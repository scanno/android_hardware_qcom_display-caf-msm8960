/*
 * Copyright (C) 2010 The Android Open Source Project
 * Copyright (C) 2012-2013, 2015 The Linux Foundation. All rights reserved.
 *
 * Not a Contribution, Apache license notifications and license are
 * retained for attribution purposes only.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define DEBUG 0
#include <ctype.h>
#include <fcntl.h>
#include <utils/threads.h>
#include <utils/Errors.h>
#include <utils/Log.h>

#include <linux/msm_mdp.h>
#include <video/msm_hdmi_modes.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <cutils/properties.h>
#include "hwc_utils.h"
#include "external.h"
#include "overlayUtils.h"
#include "overlay.h"
#include <poll.h>
#include "mdp_version.h"

using namespace android;

namespace qhwc {
#define MAX_SYSFS_FILE_PATH              255
#define MAX_SYSFS_FILE_LENGTH            512
#define MAX_HDMI_FEAT_EN_NAME_LEN        10
#define SPD_NAME_LENGTH                  16
#define HPD_STR                          "HPD"
#define EDID_STR                         "EDID"
#define UNKNOWN_STRING                   "unknown"
#define HDMI_FEAT_EN_SYSFS_PATH "/sys/devices/virtual/graphics/fb%d/hdmi_feature_en"

ExternalDisplay::ExternalDisplay(hwc_context_t* ctx, int dpy):mFd(-1),
       mHwcContext(ctx), mDpy(dpy)
{
    memset(&mVInfo, 0, sizeof(mVInfo));
    mFbNum = overlay::Overlay::getInstance()->getFbForDpy(mDpy);
}

bool ExternalDisplay::openFrameBuffer()
{
    FILE *fp = NULL;
    const int MAX_OPEN_FB_LEN = 64;
    char name[MAX_OPEN_FB_LEN] = {0};
    char disp_id_str[MAX_OPEN_FB_LEN] = {0};
    bool openFb = true;

    if (mFd == -1) {
        if(0 > mHwcContext->dpyAttr[mDpy].fd) {
            /* Look for display id first */
            snprintf(name, MAX_OPEN_FB_LEN, HWC_FB_DISP_ID_SYS_PATH, mDpy);
            fp = fopen(name, "r");
            if (!fp) {
                ALOGW("%s can't open %s", __FUNCTION__, name);
                /* If disp_id path is not existing, then fall back to legacy
                 * way, which opens fb. */
            } else {
                memset(disp_id_str, 0, sizeof(disp_id_str));
                fread(disp_id_str, sizeof(char), MAX_OPEN_FB_LEN, fp);
                /* Don't open this FB if it's not for external displays */
                if (strcmp(getHwcFbDispId(mDpy), disp_id_str)) {
                    ALOGI("%s disp_id=%s is not for external display",
                          __FUNCTION__, disp_id_str);
                    openFb = false;
                }
                fclose(fp);
            }
            if (openFb) {
                char strDevPath[MAX_SYSFS_FILE_PATH];
                snprintf(strDevPath, MAX_SYSFS_FILE_PATH, "/dev/graphics/fb%d",
                         mFbNum);
                mFd = open(strDevPath, O_RDWR);
                if (mFd < 0)
                    ALOGE("%s: %s is not available", __FUNCTION__, strDevPath);
                mHwcContext->dpyAttr[mDpy].fd = mFd;
            }
        } else {
            mFd = mHwcContext->dpyAttr[mDpy].fd;
        }
    }
    return (mFd > 0);
}

bool ExternalDisplay::closeFrameBuffer()
{
    int ret = 0;
    if(mFd >= 0) {
        ret = close(mFd);
        mFd = -1;
    }
    mHwcContext->dpyAttr[mDpy].fd = mFd;
    return (ret == 0);
}

// ---------------- SecondaryDisplay Implementation ----------------------------
SecondaryDisplay::SecondaryDisplay(hwc_context_t* ctx, int dpy):
    ExternalDisplay(ctx, dpy),
    mCurrentMode(-1), mModeCount(0),
    mUnderscanSupported(false),
    mHPDEnabled(false),
    mEDIDEnabled(false)
{
    if(ctx->mAutomotiveModeOn) {
        parseFeatures();
        ctx->mHPDEnabled = mHPDEnabled;

    if (mHPDEnabled)
        writeHPDOption(0);
    else
        writeHPDOption(1);
    } else {
        // disable HPD at start, it will be enabled later
        // when the display powers on
        // This helps for framework reboot or adb shell stop/start
        writeHPDOption(0);
    }
    if (mEDIDEnabled) {
        // for HDMI - retreive all the modes supported by the driver
        if(mFbNum != -1) {
            supported_video_mode_lut =
                 new msm_hdmi_mode_timing_info[HDMI_VFRMT_MAX];
            // Populate the mode table for supported modes
            MSM_HDMI_MODES_INIT_TIMINGS(supported_video_mode_lut);
            MSM_HDMI_MODES_SET_SUPP_TIMINGS(supported_video_mode_lut,
                                                 MSM_HDMI_MODES_ALL);
            // Update the Source Product Information
            // Vendor Name
            setSPDInfo("vendor_name", "ro.product.manufacturer");
            // Product Description
            setSPDInfo("product_description", "ro.product.name");
        }
    }
}

SecondaryDisplay::~SecondaryDisplay()
{
    delete [] supported_video_mode_lut;
    closeFrameBuffer();
}

int SecondaryDisplay::configure() {
    if(mHwcContext->mAutomotiveModeOn && !waitForConnectEvent()) {
        return -1;
    }

    if(!openFrameBuffer()) {
        ALOGE("%s: Failed to open FB: %d", __FUNCTION__, mFbNum);
        return -1;
    }

    if(!mEDIDEnabled) {
        // Get the fb_var_screeninfo and initialize mVInfo of tertiary display
        struct fb_var_screeninfo info;
        int ret = 0;
        ret = ioctl(mFd, FBIOGET_VSCREENINFO, &mVInfo);
        if(ret < 0) {
            ALOGE("In %s: FBIOGET_VSCREENINFO failed Err Str = %s", __FUNCTION__,
                                                                strerror(errno));
        }
        ALOGI("%s: GET Info<ID=%d %dx%d (%d,%d,%d),"
                "(%d,%d,%d) %dMHz>", __FUNCTION__,
                mVInfo.reserved[3], mVInfo.xres, mVInfo.yres,
                mVInfo.right_margin, mVInfo.hsync_len, mVInfo.left_margin,
                mVInfo.lower_margin, mVInfo.vsync_len, mVInfo.upper_margin,
                mVInfo.pixclock/1000/1000);
        // For secondary display, set the attributes
        setAttributes();
        property_set("hw.hdmiON", "1");
    } else {
        readCEUnderscanInfo();
        readResolution();
        // TODO: Move this to activate
        /* Used for changing the resolution
         * getUserMode will get the preferred
         * mode set thru adb shell */
        int mode = getUserMode();
        if (mode == -1) {
            //Get the best mode and set
            mode = getBestMode();
        }
        setResolution(mode);
        setAttributes();
        // set system property
        property_set("hw.hdmiON", "1");
    }
    return 0;
}

void SecondaryDisplay::getAttributes(int& width, int& height) {
    if(mHwcContext) {
        if(!mEDIDEnabled) {
            /* derive the width and height values from fb_var_screeninfo */
            width = mVInfo.xres;
            height = mVInfo.yres;
        } else {
            int fps = 0;
            getAttrForMode(width, height, fps);
        }
    }
}

int SecondaryDisplay::teardown() {
    closeFrameBuffer();
    resetInfo();
    // unset system property
    property_set("hw.hdmiON", "0");
    return 0;
}

/* gets the product manufacturer and product name and writes it
 * to the sysfs node, so that the driver can get that information
 * Used to show QCOM 8974 instead of Input 1 for example
 */
void SecondaryDisplay::setSPDInfo(const char* node, const char* property) {
    int err = -1;
    char info[PROPERTY_VALUE_MAX];
    char sysFsSPDFilePath[MAX_SYSFS_FILE_PATH];
    memset(sysFsSPDFilePath, 0, sizeof(sysFsSPDFilePath));
    snprintf(sysFsSPDFilePath , sizeof(sysFsSPDFilePath),
                 "/sys/devices/virtual/graphics/fb%d/%s",
                 mFbNum, node);
    int spdFile = open(sysFsSPDFilePath, O_RDWR, 0);
    if (spdFile < 0) {
        ALOGE("%s: file '%s' not found : ret = %d"
              "err str: %s",  __FUNCTION__, sysFsSPDFilePath,
              spdFile, strerror(errno));
    } else {
        memset(info, 0, sizeof(info));
        property_get(property, info, UNKNOWN_STRING);
        ALOGD_IF(DEBUG, "In %s: %s = %s", __FUNCTION__, property, info);
        if (strncmp(info, UNKNOWN_STRING, SPD_NAME_LENGTH)) {
            err = write(spdFile, info, strlen(info));
            if (err <= 0) {
                ALOGE("%s: file write failed for '%s'"
                      "err no = %d", __FUNCTION__, sysFsSPDFilePath, errno);
            }
        } else {
            ALOGD_IF(DEBUG, "%s: property_get failed for SPD %s",
                         __FUNCTION__, node);
        }
        close(spdFile);
    }
}

void SecondaryDisplay::setHPD(uint32_t startEnd) {
    if (mHPDEnabled) {
        ALOGD_IF(DEBUG,"HPD enabled=%d", startEnd);
        writeHPDOption(startEnd);
    } else {
        ALOGD_IF(DEBUG,"Not a HDMI display");
    }
}

void SecondaryDisplay::setActionSafeDimension(int w, int h) {
    ALOGD_IF(DEBUG,"ActionSafe w=%d h=%d", w, h);
    char actionsafeWidth[PROPERTY_VALUE_MAX];
    char actionsafeHeight[PROPERTY_VALUE_MAX];
    snprintf(actionsafeWidth, sizeof(actionsafeWidth), "%d", w);
    property_set("persist.sys.actionsafe.width", actionsafeWidth);
    snprintf(actionsafeHeight, sizeof(actionsafeHeight), "%d", h);
    property_set("persist.sys.actionsafe.height", actionsafeHeight);
}

int SecondaryDisplay::getModeCount() const {
    ALOGD_IF(DEBUG,"HPD mModeCount=%d", mModeCount);
    return mModeCount;
}

void SecondaryDisplay::getEDIDModes(int *out) const {
    for(int i = 0;i < mModeCount;i++) {
        out[i] = mEDIDModes[i];
    }
}

void SecondaryDisplay::readCEUnderscanInfo()
{
    int hdmiScanInfoFile = -1;
    int len = -1;
    char scanInfo[17];
    char *ce_info_str = NULL;
    const char token[] = ", \n";
    char *rest = NULL;
    int ce_info = -1;
    char sysFsScanInfoFilePath[MAX_SYSFS_FILE_PATH];
    snprintf(sysFsScanInfoFilePath, sizeof(sysFsScanInfoFilePath),
            "/sys/devices/virtual/graphics/fb%d/"
                                   "scan_info", mFbNum);

    memset(scanInfo, 0, sizeof(scanInfo));
    hdmiScanInfoFile = open(sysFsScanInfoFilePath, O_RDONLY, 0);
    if (hdmiScanInfoFile < 0) {
        ALOGD_IF(DEBUG, "%s: scan_info file '%s' not found",
                                __FUNCTION__, sysFsScanInfoFilePath);
        return;
    } else {
        len = read(hdmiScanInfoFile, scanInfo, sizeof(scanInfo)-1);
        ALOGD("%s: Scan Info string: %s length = %d",
                 __FUNCTION__, scanInfo, len);
        if (len <= 0) {
            close(hdmiScanInfoFile);
            ALOGE("%s: Scan Info file empty '%s'",
                                __FUNCTION__, sysFsScanInfoFilePath);
            return;
        }
        scanInfo[len] = '\0';  /* null terminate the string */
    }
    close(hdmiScanInfoFile);

    /*
     * The scan_info contains the three fields
     * PT - preferred video format
     * IT - video format
     * CE video format - containing the underscan support information
     */

    /* PT */
    ce_info_str = strtok_r(scanInfo, token, &rest);
    if (ce_info_str) {
        /* IT */
        ce_info_str = strtok_r(NULL, token, &rest);
        if (ce_info_str) {
            /* CE */
            ce_info_str = strtok_r(NULL, token, &rest);
            if (ce_info_str)
                ce_info = atoi(ce_info_str);
        }
    }

    if (ce_info_str) {
        // ce_info contains the underscan information
        if (ce_info == EXT_SCAN_ALWAYS_UNDERSCANED ||
            ce_info == EXT_SCAN_BOTH_SUPPORTED)
            // if TV supported underscan, then driver will always underscan
            // hence no need to apply action safe rectangle
            mUnderscanSupported = true;
    } else {
        ALOGE("%s: scan_info string error", __FUNCTION__);
    }

    if(!mHwcContext->mAutomotiveModeOn) {
        // Store underscan support info in a system property
        const char* prop = (mUnderscanSupported) ? "1" : "0";
        property_set("hw.underscan_supported", prop);
    }
    return;
}

/*
 * sets the fb_var_screeninfo from the hdmi_mode_timing_info
 */
void setDisplayTiming(struct fb_var_screeninfo &info,
                                const msm_hdmi_mode_timing_info* mode)
{
    info.reserved[0] = 0;
    info.reserved[1] = 0;
    info.reserved[2] = 0;
#ifndef FB_METADATA_VIDEO_INFO_CODE_SUPPORT
    info.reserved[3] = (info.reserved[3] & 0xFFFF) |
              (mode->video_format << 16);
#endif
    info.xoffset = 0;
    info.yoffset = 0;
    info.xres = mode->active_h;
    info.yres = mode->active_v;

    info.pixclock = (mode->pixel_freq)*1000;
    info.vmode = mode->interlaced ?
                    FB_VMODE_INTERLACED : FB_VMODE_NONINTERLACED;

    info.right_margin = mode->front_porch_h;
    info.hsync_len = mode->pulse_width_h;
    info.left_margin = mode->back_porch_h;
    info.lower_margin = mode->front_porch_v;
    info.vsync_len = mode->pulse_width_v;
    info.upper_margin = mode->back_porch_v;
}

int SecondaryDisplay::parseResolution(char* edidStr, int* edidModes)
{
    char delim = ',';
    int count = 0;
    char *start, *end;
    // EDIDs are string delimited by ','
    // Ex: 16,4,5,3,32,34,1
    // Parse this string to get mode(int)
    start = (char*) edidStr;
    end = &delim;
    while(*end == delim) {
        edidModes[count] = (int) strtol(start, &end, 10);
        start = end+1;
        count++;
    }
    ALOGD_IF(DEBUG, "In %s: count = %d", __FUNCTION__, count);
    for (int i = 0; i < count; i++)
        ALOGD_IF(DEBUG, "Mode[%d] = %d", i, edidModes[i]);
    return count;
}

int SecondaryDisplay::parseFeatures(void)
{
    char sysFsEDIDFilePath[MAX_SYSFS_FILE_PATH];
    const char delim1[] = ":";
    const char delim2[] = ";";
    char *tmp = NULL, *cont = NULL;
    int len = -1;
    char featEnStr[MAX_SYSFS_FILE_LENGTH] = {'\0'};
    int hdmiFeaturesFile;

    snprintf(sysFsEDIDFilePath, sizeof(sysFsEDIDFilePath),
                          HDMI_FEAT_EN_SYSFS_PATH, mFbNum);

    hdmiFeaturesFile = open(sysFsEDIDFilePath, O_RDONLY, 0);

    if (hdmiFeaturesFile < 0) {
       ALOGE("HDMI_FEAT %s: hdmi features enabled file '%s' not found",
                                           __FUNCTION__, sysFsEDIDFilePath);
       return 0;
    } else {
        len = read(hdmiFeaturesFile, featEnStr, sizeof(featEnStr)-1);
        if (len <= 0) {
            ALOGE("HDMI_FEAT %s: hdmi features enabled file empty '%s'",
                                            __FUNCTION__, sysFsEDIDFilePath);
            featEnStr[0] = '\0';
            mHPDEnabled = false;
            mEDIDEnabled = false;
            close(hdmiFeaturesFile);
            return 0;
        }
    }
    close(hdmiFeaturesFile);

    tmp = strtok_r(featEnStr, delim1, &cont);
    while(tmp != NULL) {
        if (strncmp(tmp, HPD_STR, MAX_HDMI_FEAT_EN_NAME_LEN) == 0) {
            tmp = strtok_r(NULL, delim2, &cont);
            if (tmp == NULL)
                break;
            mHPDEnabled = atoi(tmp);
        } else if (strncmp(tmp, EDID_STR, MAX_HDMI_FEAT_EN_NAME_LEN) == 0) {
            tmp = strtok_r(NULL, delim2, &cont);
            if (tmp == NULL)
                break;
            mEDIDEnabled = atoi(tmp);
        } else {
            ALOGE("HDMI_FEAT %s: hdmi feature unknown '%s' sys node string: %s",
                                              __FUNCTION__, tmp, featEnStr);
            break;
        }
        if (tmp == NULL)
            break;
        tmp = strtok_r(NULL, delim1, &cont);
    }

    return 0;
}

bool SecondaryDisplay::readResolution()
{
    char sysFsEDIDFilePath[MAX_SYSFS_FILE_PATH];
    snprintf(sysFsEDIDFilePath , sizeof(sysFsEDIDFilePath),
            "/sys/devices/virtual/graphics/fb%d/edid_modes", mFbNum);

    int hdmiEDIDFile = open(sysFsEDIDFilePath, O_RDONLY, 0);
    int len = -1;
    char edidStr[128] = {'\0'};

    if (hdmiEDIDFile < 0) {
        ALOGE("%s: edid_modes file '%s' not found",
                 __FUNCTION__, sysFsEDIDFilePath);
        return false;
    } else {
        len = read(hdmiEDIDFile, edidStr, sizeof(edidStr)-1);
        ALOGD_IF(DEBUG, "%s: EDID string: %s length = %d",
                 __FUNCTION__, edidStr, len);
        if ( len <= 0) {
            ALOGE("%s: edid_modes file empty '%s'",
                     __FUNCTION__, sysFsEDIDFilePath);
            edidStr[0] = '\0';
        }
        else {
            while (len > 1 && isspace(edidStr[len-1])) {
                --len;
            }
            edidStr[len] = '\0';
        }
    }
    close(hdmiEDIDFile);
    if(len > 0) {
        // Get EDID modes from the EDID strings
        mModeCount = parseResolution(edidStr, mEDIDModes);
        ALOGD_IF(DEBUG, "%s: mModeCount = %d", __FUNCTION__,
                 mModeCount);
    }

    return (len > 0);
}

bool SecondaryDisplay::waitForConnectEvent() {
    if (mHPDEnabled) {
        // Open a sysfs node to receive the timeout notification from driver.
        char sysFsExtConnectionPath[MAX_SYSFS_FILE_PATH];
        snprintf(sysFsExtConnectionPath , sizeof(sysFsExtConnectionPath),
                     "/sys/devices/virtual/graphics/fb%d/connected", mFbNum);
        int fd = open(sysFsExtConnectionPath, O_RDONLY);
        if (fd < 0) {
            ALOGE ("%s:not able to open %s node %s",
                    __FUNCTION__, sysFsExtConnectionPath, strerror(errno));
            return false;
        }
        struct pollfd pFd;
        pFd.fd = fd;
        if (pFd.fd >= 0)
            pFd.events = POLLPRI | POLLERR;
        do {
            // Poll for an HDMI connect event from driver
            int err = poll(&pFd, 1, 1000);
            if(err > 0) {
                if (pFd.revents & POLLPRI) {
                    char status[64];
                    // Consume the node by reading it
                    ssize_t len = pread(pFd.fd, status, 64, 0);
                    if (UNLIKELY(len < 0)) {
                        // If the read was just interrupted - it is not a
                        // fatal error. Just continue in this case
                        ALOGE ("%s: Unable to read connect event for secondary: %s",
                                __FUNCTION__, strerror(errno));
                        continue;
                    }
                    // extract connection status
                    if (!strncmp(status, "1", strlen("1"))) {
                        close(fd);
                        return true;
                    } else {
                        close(fd);
                        return false;
                    }
                }
            }else {
                close(fd);
                return false;
            }
        }while(true);
    } else {
        // Panel is always connected if not a hdmi display
        return true;
    }

}

// clears the vinfo, edid, best modes
void SecondaryDisplay::resetInfo()
{
    memset(&mVInfo, 0, sizeof(mVInfo));
    memset(mEDIDModes, 0, sizeof(mEDIDModes));
    mModeCount = 0;
    mCurrentMode = -1;
    mUnderscanSupported = false;
    // Reset the underscan supported system property
    const char* prop = "0";
    property_set("hw.underscan_supported", prop);
}

int SecondaryDisplay::getModeOrder(int mode)
{
    // XXX: We dont support interlaced modes but having
    // it here for future
    switch (mode) {
        default:
        case HDMI_VFRMT_1440x480i60_4_3:
            return 1; // 480i 4:3
        case HDMI_VFRMT_1440x480i60_16_9:
            return 2; // 480i 16:9
        case HDMI_VFRMT_1440x576i50_4_3:
            return 3; // i576i 4:3
        case HDMI_VFRMT_1440x576i50_16_9:
            return 4; // 576i 16:9
        case HDMI_VFRMT_1920x1080i60_16_9:
            return 5; // 1080i 16:9
        case HDMI_VFRMT_640x480p60_4_3:
            return 6; // 640x480 4:3
        case HDMI_VFRMT_720x480p60_4_3:
            return 7; // 480p 4:3
        case HDMI_VFRMT_720x480p60_16_9:
            return 8; // 480p 16:9
        case HDMI_VFRMT_720x576p50_4_3:
            return 9; // 576p 4:3
        case HDMI_VFRMT_720x576p50_16_9:
            return 10; // 576p 16:9
        case HDMI_VFRMT_1280x1024p60_5_4:
            return 11; // 1024p; Vesa format
        case HDMI_VFRMT_1280x720p50_16_9:
            return 12; // 720p@50Hz
        case HDMI_VFRMT_1280x720p60_16_9:
            return 13; // 720p@60Hz
        case HDMI_VFRMT_1920x1080p24_16_9:
            return 14; //1080p@24Hz
        case HDMI_VFRMT_1920x1080p25_16_9:
            return 15; //108-p@25Hz
        case HDMI_VFRMT_1920x1080p30_16_9:
            return 16; //1080p@30Hz
        case HDMI_VFRMT_1920x1080p50_16_9:
            return 17; //1080p@50Hz
        case HDMI_VFRMT_1920x1080p60_16_9:
            return 18; //1080p@60Hz
        case HDMI_VFRMT_2560x1600p60_16_9:
            return 19; //WQXGA@60Hz541
        case HDMI_VFRMT_3840x2160p24_16_9:
            return 20;//2160@24Hz
        case HDMI_VFRMT_3840x2160p25_16_9:
            return 21;//2160@25Hz
        case HDMI_VFRMT_3840x2160p30_16_9:
            return 22; //2160@30Hz
        case HDMI_VFRMT_4096x2160p24_16_9:
            return 23; //4kx2k@24Hz
    }
}

/// Returns the user mode set(if any) using adb shell
int SecondaryDisplay::getUserMode() {
    /* Based on the property set the resolution */
    char property_value[PROPERTY_VALUE_MAX];
    property_get("hw.hdmi.resolution", property_value, "-1");
    int mode = atoi(property_value);
    // We dont support interlaced modes
    if(isValidMode(mode) && !isInterlacedMode(mode)) {
        ALOGD_IF(DEBUG, "%s: setting the HDMI mode = %d", __FUNCTION__, mode);
        return mode;
    }
    return -1;
}

// Get the best mode for the current HD TV
int SecondaryDisplay::getBestMode() {
    int bestOrder = 0;
    int bestMode = HDMI_VFRMT_640x480p60_4_3;
    // for all the edid read, get the best mode
    for(int i = 0; i < mModeCount; i++) {
        int mode = mEDIDModes[i];
        int order = getModeOrder(mode);
        if (order > bestOrder) {
            bestOrder = order;
            bestMode = mode;
        }
    }
    return bestMode;
}

inline bool SecondaryDisplay::isValidMode(int ID)
{
    bool valid = false;
    for (int i = 0; i < mModeCount; i++) {
        if(ID == mEDIDModes[i]) {
            valid = true;
            break;
        }
    }
    return valid;
}

// returns true if the mode(ID) is interlaced mode format
bool SecondaryDisplay::isInterlacedMode(int ID) {
    bool interlaced = false;
    switch(ID) {
        case HDMI_VFRMT_1440x480i60_4_3:
        case HDMI_VFRMT_1440x480i60_16_9:
        case HDMI_VFRMT_1440x576i50_4_3:
        case HDMI_VFRMT_1440x576i50_16_9:
        case HDMI_VFRMT_1920x1080i60_16_9:
            interlaced = true;
        default:
            interlaced = false;
    }
    return interlaced;
}

bool SecondaryDisplay::writeHPDOption(int userOption) const
{
    bool ret = true;
    if(mFbNum != -1) {
        char sysFsHPDFilePath[MAX_SYSFS_FILE_PATH];
        snprintf(sysFsHPDFilePath ,sizeof(sysFsHPDFilePath),
                "/sys/devices/virtual/graphics/fb%d/hpd", mFbNum);
        int hdmiHPDFile = open(sysFsHPDFilePath,O_RDWR, 0);
        if (hdmiHPDFile < 0) {
            ALOGE("%s: state file '%s' not found : ret%d err str: %s",
                  __FUNCTION__, sysFsHPDFilePath, hdmiHPDFile, strerror(errno));
            ret = false;
        } else {
            int err = -1;
            ALOGD_IF(DEBUG, "%s: option = %d", __FUNCTION__, userOption);
            if(userOption)
                err = write(hdmiHPDFile, "1", 2);
            else
                err = write(hdmiHPDFile, "0" , 2);
            if (err <= 0) {
                ALOGE("%s: file write failed '%s'", __FUNCTION__,
                        sysFsHPDFilePath);
                ret = false;
            }
            close(hdmiHPDFile);
        }
    }
    return ret;
}

void SecondaryDisplay::setResolution(int ID)
{
    struct fb_var_screeninfo info;
    int ret = 0;
    ret = ioctl(mFd, FBIOGET_VSCREENINFO, &mVInfo);
    if(ret < 0) {
        ALOGD("In %s: FBIOGET_VSCREENINFO failed Err Str = %s", __FUNCTION__,
                                                            strerror(errno));
    }
    ALOGD_IF(DEBUG, "%s: GET Info<ID=%d %dx%d (%d,%d,%d),"
            "(%d,%d,%d) %dMHz>", __FUNCTION__,
            mVInfo.reserved[3], mVInfo.xres, mVInfo.yres,
            mVInfo.right_margin, mVInfo.hsync_len, mVInfo.left_margin,
            mVInfo.lower_margin, mVInfo.vsync_len, mVInfo.upper_margin,
            mVInfo.pixclock/1000/1000);
    //If its a new ID - update var_screeninfo
    if ((isValidMode(ID)) && mCurrentMode != ID) {
        const struct msm_hdmi_mode_timing_info *mode =
            &supported_video_mode_lut[0];
        for (unsigned int i = 0; i < HDMI_VFRMT_MAX; ++i) {
            const struct msm_hdmi_mode_timing_info *cur =
                                        &supported_video_mode_lut[i];
            if (cur->video_format == (uint32_t)ID) {
                mode = cur;
                break;
            }
        }
        setDisplayTiming(mVInfo, mode);
        ALOGD_IF(DEBUG, "%s: SET Info<ID=%d => Info<ID=%d %dx %d"
                 "(%d,%d,%d), (%d,%d,%d) %dMHz>", __FUNCTION__, ID,
                 mode->video_format, mVInfo.xres, mVInfo.yres,
                 mVInfo.right_margin, mVInfo.hsync_len, mVInfo.left_margin,
                 mVInfo.lower_margin, mVInfo.vsync_len, mVInfo.upper_margin,
                 mVInfo.pixclock/1000/1000);
#ifdef FB_METADATA_VIDEO_INFO_CODE_SUPPORT
        struct msmfb_metadata metadata;
        memset(&metadata, 0 , sizeof(metadata));
        metadata.op = metadata_op_vic;
        metadata.data.video_info_code = mode->video_format;
        if (ioctl(mFd, MSMFB_METADATA_SET, &metadata) == -1) {
            ALOGD("In %s: MSMFB_METADATA_SET failed Err Str = %s",
                                                 __FUNCTION__, strerror(errno));
        }
#endif
        mVInfo.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_ALL | FB_ACTIVATE_FORCE;
        ret = ioctl(mFd, FBIOPUT_VSCREENINFO, &mVInfo);
        if(ret < 0) {
            ALOGD("In %s: FBIOPUT_VSCREENINFO failed Err Str = %s",
                                                 __FUNCTION__, strerror(errno));
        }
        mCurrentMode = ID;
    }
}

void SecondaryDisplay::setAttributes() {
    int width = 0, height = 0, fps = 60;
    getAttributes(width, height);
    ALOGD("SecondaryDisplay setting xres = %d, yres = %d", width, height);
    if(mHwcContext) {
        if(!mEDIDEnabled) {
            // Always set dpyAttr res to mVInfo res
            mHwcContext->dpyAttr[mDpy].xres = width;
            mHwcContext->dpyAttr[mDpy].yres = height;
            mHwcContext->dpyAttr[mDpy].mDownScaleMode = false;
            mHwcContext->dpyAttr[mDpy].vsync_period =
                    1000000000l / fps;
        } else {
            // Always set dpyAttr res to mVInfo res
            mHwcContext->dpyAttr[mDpy].xres = width;
            mHwcContext->dpyAttr[mDpy].yres = height;
            mHwcContext->dpyAttr[mDpy].mDownScaleMode = false;
            int priW = mHwcContext->dpyAttr[HWC_DISPLAY_PRIMARY].xres;
            int priH = mHwcContext->dpyAttr[HWC_DISPLAY_PRIMARY].yres;
            // if primary resolution is more than HDMI resolution and
            // downscale_factor is zero(which corresponds to downscale
            // to > 50% of orig),then configure dpy attr to primary
            // resolution and set downscale mode.
            if(((priW * priH) > (width * height)) &&
                (priW <= MAX_DISPLAY_DIM )) {
                int downscale_factor =
                    overlay::utils::getDownscaleFactor(priW, priH, width, height);
                if(!downscale_factor) {
                    mHwcContext->dpyAttr[mDpy].xres = priW;
                    mHwcContext->dpyAttr[mDpy].yres = priH;
                    // HDMI is always in landscape, so always assign the higher
                    // dimension to hdmi's xres
                    if(priH > priW) {
                        mHwcContext->dpyAttr[mDpy].xres = priH;
                        mHwcContext->dpyAttr[mDpy].yres = priW;
                    }
                    // Set Secondary Display MDP Downscale mode indicator
                    mHwcContext->dpyAttr[mDpy].mDownScaleMode =true;
                }
            }
            mHwcContext->dpyAttr[mDpy].vsync_period =
                    1000000000l / fps;
        }
    }
}

void SecondaryDisplay::getAttrForMode(int& width, int& height, int& fps) {
    switch (mCurrentMode) {
        case HDMI_VFRMT_640x480p60_4_3:
            width = 640;
            height = 480;
            fps = 60;
            break;
        case HDMI_VFRMT_720x480p60_4_3:
        case HDMI_VFRMT_720x480p60_16_9:
            width = 720;
            height = 480;
            fps = 60;
            break;
        case HDMI_VFRMT_720x576p50_4_3:
        case HDMI_VFRMT_720x576p50_16_9:
            width = 720;
            height = 576;
            fps = 50;
            break;
        case HDMI_VFRMT_1280x720p50_16_9:
            width = 1280;
            height = 720;
            fps = 50;
            break;
        case HDMI_VFRMT_1280x720p60_16_9:
            width = 1280;
            height = 720;
            fps = 60;
            break;
        case HDMI_VFRMT_1280x1024p60_5_4:
            width = 1280;
            height = 1024;
            fps = 60;
            break;
        case HDMI_VFRMT_1920x1080p24_16_9:
            width = 1920;
            height = 1080;
            fps = 24;
            break;
        case HDMI_VFRMT_1920x1080p25_16_9:
            width = 1920;
            height = 1080;
            fps = 25;
            break;
        case HDMI_VFRMT_1920x1080p30_16_9:
            width = 1920;
            height = 1080;
            fps = 30;
            break;
        case HDMI_VFRMT_1920x1080p50_16_9:
            width = 1920;
            height = 1080;
            fps = 50;
            break;
        case HDMI_VFRMT_1920x1080p60_16_9:
            width = 1920;
            height = 1080;
            fps = 60;
            break;
        case HDMI_VFRMT_2560x1600p60_16_9:
            width = 2560;
            height = 1600;
            fps = 60;
            break;
        case HDMI_VFRMT_3840x2160p24_16_9:
            width = 3840;
            height = 2160;
            fps = 24;
            break;
        case HDMI_VFRMT_3840x2160p30_16_9:
            width = 3840;
            height = 2160;
            fps = 30;
            break;
        case HDMI_VFRMT_4096x2160p24_16_9:
            width = 4096;
            height = 2160;
            fps = 24;
            break;
        case HDMI_VFRMT_1280x800p60_16_9:
            width = 1280;
            height = 800;
            fps = 60;
            break;
        default:
            width = 0;
            height = 0;
            fps = 0;
            ALOGE("%s: missing mode details.. check..:mode:%d", __FUNCTION__, mCurrentMode);
            break;
    }
}

// ---------------- TertiaryDisplay Implementation ----------------------------
TertiaryDisplay::TertiaryDisplay(hwc_context_t* ctx, int dpy):
    ExternalDisplay(ctx, dpy)
{
}

TertiaryDisplay::~TertiaryDisplay()
{
    closeFrameBuffer();
}

int TertiaryDisplay::configure() {
    if(!openFrameBuffer()) {
        ALOGE("%s: Failed to open FB: %d", __FUNCTION__, mFbNum);
        return -1;
    }
    // Get the fb_var_screeninfo and initialize mVInfo of tertiary display
    struct fb_var_screeninfo info;
    int ret = 0;
    ret = ioctl(mFd, FBIOGET_VSCREENINFO, &mVInfo);
    if(ret < 0) {
        ALOGD("In %s: FBIOGET_VSCREENINFO failed Err Str = %s", __FUNCTION__,
                                                            strerror(errno));
    }
    ALOGD_IF(DEBUG, "%s: GET Info<ID=%d %dx%d (%d,%d,%d),"
            "(%d,%d,%d) %dMHz>", __FUNCTION__,
            mVInfo.reserved[3], mVInfo.xres, mVInfo.yres,
            mVInfo.right_margin, mVInfo.hsync_len, mVInfo.left_margin,
            mVInfo.lower_margin, mVInfo.vsync_len, mVInfo.upper_margin,
            mVInfo.pixclock/1000/1000);
    // For third display, set the attributes
    setAttributes();
    return 0;
}

void TertiaryDisplay::getAttributes(int& width, int& height) {
    // derive the width and height values from fb_var_screeninfo
    width = mVInfo.xres;
    height = mVInfo.yres;
}

int TertiaryDisplay::teardown() {
    closeFrameBuffer();
    return 0;
}

void TertiaryDisplay::setAttributes() {
    int width = 0, height = 0, fps = 60;
    getAttributes(width, height);
    ALOGD("TertiaryDisplay setting xres = %d, yres = %d", width, height);
    if(mHwcContext) {
        // Always set dpyAttr res to mVInfo res
        mHwcContext->dpyAttr[mDpy].xres = width;
        mHwcContext->dpyAttr[mDpy].yres = height;
        mHwcContext->dpyAttr[mDpy].mDownScaleMode = false;
        mHwcContext->dpyAttr[mDpy].vsync_period =
                1000000000l / fps;
    }
}


};
