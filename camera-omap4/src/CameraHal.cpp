/*
 * Copyright (C) Texas Instruments - http://www.ti.com/
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */
/**
* @file CameraHal.cpp
*
* This file maps the Camera Hardware Interface to V4L2.
*
*/

#define LOG_TAG "CameraHal"

#include "CameraHal.h"
#include "OverlayDisplayAdapter.h"
#include "TICameraParameters.h"
#include "CameraProperties.h"
#include "overlay_common.h"

#include <poll.h>
#include <math.h>

#define ADAPTER_TIMEOUT 5 //[s.]

///@remarks added for testing purposes

namespace android {
/*****************************************************************************/

wp<CameraHardwareInterface> CameraHal::singleton;

////Constant definitions and declarations
////@todo Have a CameraProperties class to store these parameters as constants for every camera
////       Currently, they are hard-coded

const int CameraHal::NO_BUFFERS_PREVIEW = MAX_CAMERA_BUFFERS;
const int CameraHal::NO_BUFFERS_IMAGE_CAPTURE = 2;

const uint32_t MessageNotifier::EVENT_BIT_FIELD_POSITION = 0;
const uint32_t MessageNotifier::FRAME_BIT_FIELD_POSITION = 0;

const char CameraHal::PARAMS_DELIMITER []= ",";

/******************************************************************************/

#if PPM_INSTRUMENTATION || PPM_INSTRUMENTATION_ABS

struct timeval CameraHal::ppm_start;
struct timeval CameraHal::mStartPreview;
struct timeval CameraHal::mStartFocus;
struct timeval CameraHal::mStartCapture;

#endif

/*-------------Camera Hal Interface Method definitions STARTS here--------------------*/

/**
   @brief Return the IMemoryHeap for the preview image heap

   @param none
   @return NULL
   @remarks This interface is not supported
   @note This function can be useful for Surface flinger render cases
   @todo Support this function

 */
sp<IMemoryHeap> CameraHal::getPreviewHeap() const
    {
    LOG_FUNCTION_NAME
    return ((mAppCallbackNotifier.get())?mAppCallbackNotifier->getPreviewHeap():NULL);
    }

/**
   @brief Return the IMemoryHeap for the raw image heap

     @param none
     @return NULL
     @remarks This interface is not supported
     @todo Investigate why this function might be needed

*/
sp<IMemoryHeap>  CameraHal::getRawHeap() const
{
    LOG_FUNCTION_NAME
    return NULL;
}

/**
   @brief Set the notification and data callbacks

   @param[in] notify_cb Notify callback for notifying the app about events and errors
   @param[in] data_cb   Buffer callback for sending the preview/raw frames to the app
   @param[in] data_cb_timestamp Buffer callback for sending the video frames w/ timestamp
   @param[in] user  Callback cookie
   @return none

 */
void CameraHal::setCallbacks(notify_callback notify_cb,
                                      data_callback data_cb,
                                      data_callback_timestamp data_cb_timestamp,
                                      void* user)
{
    LOG_FUNCTION_NAME

    if ( NULL != mAppCallbackNotifier.get() )
        {
            mAppCallbackNotifier->setCallbacks(this,
                                                                        notify_cb,
                                                                        data_cb,
                                                                        data_cb_timestamp,
                                                                        user);
        }

    LOG_FUNCTION_NAME_EXIT
}

/**
   @brief Enable a message, or set of messages.

   @param[in] msgtype Bitmask of the messages to enable (defined in include/ui/Camera.h)
   @return none

 */
void CameraHal::enableMsgType(int32_t msgType)
{
    LOG_FUNCTION_NAME

    if ( ( msgType & CAMERA_MSG_SHUTTER ) && ( !mShutterEnabled ) )
        {
        msgType &= ~CAMERA_MSG_SHUTTER;
        }

    {
    Mutex::Autolock lock(mLock);
    mMsgEnabled |= msgType;
    }

    if(mMsgEnabled &CAMERA_MSG_PREVIEW_FRAME)
        {
        CAMHAL_LOGDA("Enabling Preview Callback");
        }
    else
        {
        CAMHAL_LOGDB("Preview callback not enabled %x", msgType);
        }


    ///Configure app callback notifier with the message callback required
    mAppCallbackNotifier->enableMsgType (msgType);

    ///@todo once preview callback is supported, enable preview here if preview start in progress
    ///@todo Once the bug in OMXCamera related to flush buffers is removed, and external buffering
    ////in overlay is supported, we can start preview here

    LOG_FUNCTION_NAME_EXIT
}

/**
   @brief Disable a message, or set of messages.

   @param[in] msgtype Bitmask of the messages to disable (defined in include/ui/Camera.h)
   @return none

 */
void CameraHal::disableMsgType(int32_t msgType)
{
    LOG_FUNCTION_NAME

        {
        Mutex::Autolock lock(mLock);
        mMsgEnabled &= ~msgType;
        }

    if( msgType & CAMERA_MSG_PREVIEW_FRAME)
        {
        CAMHAL_LOGDA("Disabling Preview Callback");
        }

    ///Configure app callback notifier
    mAppCallbackNotifier->disableMsgType (msgType);

    LOG_FUNCTION_NAME_EXIT
}

/**
   @brief Query whether a message, or a set of messages, is enabled.

   Note that this is operates as an AND, if any of the messages queried are off, this will
   return false.

   @param[in] msgtype Bitmask of the messages to query (defined in include/ui/Camera.h)
   @return true If all message types are enabled
          false If any message type

 */
bool CameraHal::msgTypeEnabled(int32_t msgType)
{
    LOG_FUNCTION_NAME
    Mutex::Autolock lock(mLock);
    LOG_FUNCTION_NAME_EXIT
    return (mMsgEnabled & msgType);
}

/**
   @brief Set the camera parameters.

   @param[in] params Camera parameters to configure the camera
   @return NO_ERROR
   @todo Define error codes

 */
status_t CameraHal::setParameters(const CameraParameters &params)
{

   LOG_FUNCTION_NAME

    int w, h;
    int w_orig, h_orig;
    int framerate;
    int error;
    int base;
    const char *valstr = NULL;
    const char *prevFormat;
    char *af_coord;
    Message msg;
    status_t ret = NO_ERROR;

    Mutex::Autolock lock(mLock);

    ///Sensor selection will go in first as all other parameter setting is dependent on it
    if(( (valstr = params.get(TICameraParameters::KEY_CAMERA)) != NULL)
      && (params.getInt(TICameraParameters::KEY_CAMERA) >=0)
      && (params.getInt(TICameraParameters::KEY_CAMERA) < mCameraProperties->camerasSupported() ))
         {
         mParameters.set(TICameraParameters::KEY_CAMERA, valstr);
         }


    int camera_index = mParameters.getInt(TICameraParameters::KEY_CAMERA);

    CAMHAL_LOGDB("Camera Selected %s",mParameters.get(TICameraParameters::KEY_CAMERA));
    CAMHAL_LOGDB("camera_index %d, mCameraIndex %d", camera_index, mCameraIndex);

    if ( ( -1 != camera_index ) && ( camera_index != mCameraIndex ) )
        {
        mCameraIndex = camera_index;
        mReloadAdapter = true;
        }

    //This can only happen when there is a mismatch
    //between the Camera application default
    //CameraHal's own default at the start
    if ( mReloadAdapter )
        {
         if ( NULL != mCameraAdapter )
            {
             // Free the camera adapter
             //mCameraAdapter.clear();

             //Close the camera adapter DLL
             //::dlclose(mCameraAdapterHandle);
            }

        if ( reloadAdapter() < 0 )
            {
            CAMHAL_LOGEA("CameraAdapter reload failed");
            }

        mReloadAdapter = false;
        }
    ///Ensure that preview is not enabled when the below parameters are changed.
    if(!previewEnabled())
        {


        CAMHAL_LOGDB("PreviewFormat %s", params.getPreviewFormat());

        if ( !isParameterValid(params.getPreviewFormat(), (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_PREVIEW_FORMATS]->mPropValue))
            {
            CAMHAL_LOGEB("Invalid preview format %s",  (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_PREVIEW_FORMATS]->mPropValue);
            ret = -EINVAL;
            }
        else
          {
            if ( (valstr = params.getPreviewFormat()) != NULL)
              mParameters.setPreviewFormat(valstr);
          }

        params.getPreviewSize(&w, &h);
        if ( !isResolutionValid(w, h, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_PREVIEW_SIZES]->mPropValue))
            {
            CAMHAL_LOGEB("Invalid preview resolution %d x %d", w, h);
            ret = -EINVAL;
            }
        else
            {
            mParameters.setPreviewSize(w, h);
            }

        CAMHAL_LOGDB("PreviewResolution by App %d x %d", w, h);

        if(( (valstr = params.get(TICameraParameters::KEY_VNF)) != NULL)
            && ((params.getInt(TICameraParameters::KEY_VNF)==0) || (params.getInt(TICameraParameters::KEY_VNF)==1)))
            {
            CAMHAL_LOGDB("VNF set %s", params.get(TICameraParameters::KEY_VNF));
            mParameters.set(TICameraParameters::KEY_VNF, valstr);
            }

        if(( (valstr = params.get(TICameraParameters::KEY_VSTAB)) != NULL)
             && ((params.getInt(TICameraParameters::KEY_VSTAB)==0) || (params.getInt(TICameraParameters::KEY_VSTAB)==1)))
            {
            CAMHAL_LOGDB("VSTAB set %s", params.get(TICameraParameters::KEY_VSTAB));
            mParameters.set(TICameraParameters::KEY_VSTAB, valstr);
            }

        if( (valstr = params.get(TICameraParameters::KEY_CAP_MODE)) != NULL)
            {
            CAMHAL_LOGDB("Capture mode set %s", params.get(TICameraParameters::KEY_CAP_MODE));
            mParameters.set(TICameraParameters::KEY_CAP_MODE, valstr);
            }

        if((valstr = params.get(TICameraParameters::KEY_IPP)) != NULL)
            {
            CAMHAL_LOGDB("IPP mode set %s", params.get(TICameraParameters::KEY_IPP));
            mParameters.set(TICameraParameters::KEY_IPP, valstr);
            }

        if((valstr = params.get(TICameraParameters::KEY_S3D2D_PREVIEW)) != NULL)
            {
            CAMHAL_LOGDB("Stereo 3D->2D Preview mode is %s", params.get(TICameraParameters::KEY_S3D2D_PREVIEW));
            mParameters.set(TICameraParameters::KEY_S3D2D_PREVIEW, valstr);
            }

        if((valstr = params.get(TICameraParameters::KEY_AUTOCONVERGENCE)) != NULL)
            {
            CAMHAL_LOGEB("AutoConvergence mode is %s", params.get(TICameraParameters::KEY_AUTOCONVERGENCE));
            mParameters.set(TICameraParameters::KEY_AUTOCONVERGENCE, valstr);
            }

        }

    ///Below parameters can be changed when the preview is running
    if ( !isParameterValid(params.getPictureFormat(),
        (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_PICTURE_FORMATS]->mPropValue))
        {
        CAMHAL_LOGEA("Invalid picture format");
        ret = -EINVAL;
        }
    else
        {
        mParameters.setPictureFormat(params.getPictureFormat());
        }

    params.getPictureSize(&w, &h);
    if ( !isResolutionValid(w, h, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_PICTURE_SIZES]->mPropValue))
        {
        CAMHAL_LOGEA("Invalid picture resolution, exiting");
        ret = -EINVAL;
        }
    else
        {
        mParameters.setPictureSize(w, h);
        }

    CAMHAL_LOGDB("Picture Size by App %d x %d", w, h);

    if(( (valstr = params.get(TICameraParameters::KEY_BURST)) != NULL)
        && (params.getInt(TICameraParameters::KEY_BURST) >=0))
        {
        CAMHAL_LOGEB("Burst set %s", params.get(TICameraParameters::KEY_BURST));
        mParameters.set(TICameraParameters::KEY_BURST, valstr);
        }

    framerate = params.getPreviewFrameRate();
    if ( !isParameterValid(framerate, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_PREVIEW_FRAME_RATES]->mPropValue))
        {
        CAMHAL_LOGEA("Invalid frame rate");
        ret = -EINVAL;
        }
    else
        {
        mParameters.setPreviewFrameRate(framerate);
        }

    CAMHAL_LOGEB("FRAMERATE %d", framerate);

    ///Update the current parameter set
    if( (valstr = params.get(TICameraParameters::KEY_AUTOCONVERGENCE)) != NULL)
        {
        CAMHAL_LOGDB("AutoConvergence Mode is set = %s", params.get(TICameraParameters::KEY_AUTOCONVERGENCE));
        mParameters.set(TICameraParameters::KEY_AUTOCONVERGENCE, valstr);
        }

//    if(params.get(TICameraParameters::KEY_AUTOCONVERGENCE_MODE)!=NULL)
//        {
//        CAMHAL_LOGDB("AutoConvergence Mode is set = %s", params.get(TICameraParameters::KEY_AUTOCONVERGENCE_MODE));
//        mParameters.set(TICameraParameters::KEY_AUTOCONVERGENCE_MODE, params.get(TICameraParameters::KEY_AUTOCONVERGENCE_MODE));
//        }

    if( (valstr = params.get(TICameraParameters::KEY_MANUALCONVERGENCE_VALUES)) !=NULL )
        {
        CAMHAL_LOGDB("ManualConvergence Value = %s", params.get(TICameraParameters::KEY_MANUALCONVERGENCE_VALUES));
        mParameters.set(TICameraParameters::KEY_MANUALCONVERGENCE_VALUES, valstr);
        }

    if( ((valstr = params.get(TICameraParameters::KEY_EXPOSURE_MODE)) != NULL)
        && isParameterValid(params.get(TICameraParameters::KEY_EXPOSURE_MODE),
        (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_EXPOSURE_MODES]->mPropValue))
        {
        CAMHAL_LOGDB("Exposure set = %s", params.get(TICameraParameters::KEY_EXPOSURE_MODE));
        mParameters.set(TICameraParameters::KEY_EXPOSURE_MODE, valstr);
        }

    if( ((valstr = params.get(CameraParameters::KEY_WHITE_BALANCE)) != NULL)
        && isParameterValid(params.get(CameraParameters::KEY_WHITE_BALANCE),
        (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_WHITE_BALANCE]->mPropValue))
        {
        CAMHAL_LOGDB("White balance set %s", params.get(CameraParameters::KEY_WHITE_BALANCE));
        mParameters.set(CameraParameters::KEY_WHITE_BALANCE, valstr);
        }

    if( ((valstr = params.get(TICameraParameters::KEY_CONTRAST)) != NULL)
            && (params.getInt(TICameraParameters::KEY_CONTRAST) >= 0 ))
        {
        CAMHAL_LOGDB("Contrast set %s", params.get(TICameraParameters::KEY_CONTRAST));
        mParameters.set(TICameraParameters::KEY_CONTRAST, valstr);
        }

    if( ((valstr =params.get(TICameraParameters::KEY_SHARPNESS)) != NULL) && params.getInt(TICameraParameters::KEY_SHARPNESS) >= 0 )
        {
        CAMHAL_LOGDB("Sharpness set %s", params.get(TICameraParameters::KEY_SHARPNESS));
        mParameters.set(TICameraParameters::KEY_SHARPNESS, valstr);
        }


    if( ((valstr = params.get(TICameraParameters::KEY_SATURATION)) != NULL)
        && (params.getInt(TICameraParameters::KEY_SATURATION) >= 0 ) )
        {
        CAMHAL_LOGDB("Saturation set %s", params.get(TICameraParameters::KEY_SATURATION));
        mParameters.set(TICameraParameters::KEY_SATURATION, valstr);
        }

    if( ((valstr = params.get(TICameraParameters::KEY_BRIGHTNESS)) != NULL)
        && (params.getInt(TICameraParameters::KEY_BRIGHTNESS) >= 0 ))
        {
        CAMHAL_LOGDB("Brightness set %s", params.get(TICameraParameters::KEY_BRIGHTNESS));
        mParameters.set(TICameraParameters::KEY_BRIGHTNESS, valstr);
        }


    if( ((valstr = params.get(CameraParameters::KEY_ANTIBANDING)) != NULL)
        && isParameterValid(params.get(CameraParameters::KEY_ANTIBANDING),
        (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_ANTIBANDING]->mPropValue))
        {
        CAMHAL_LOGDB("Antibanding set %s", params.get(CameraParameters::KEY_ANTIBANDING));
        mParameters.set(CameraParameters::KEY_ANTIBANDING, valstr);
        }

    if( ((valstr = params.get(TICameraParameters::KEY_ISO)) != NULL)
        && isParameterValid(params.get(TICameraParameters::KEY_ISO),
        (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_ISO_VALUES]->mPropValue))
        {
        CAMHAL_LOGDB("ISO set %s", params.get(TICameraParameters::KEY_ISO));
        mParameters.set(TICameraParameters::KEY_ISO, valstr);
        }

    if( ((valstr = params.get(CameraParameters::KEY_FOCUS_MODE)) != NULL)
        && isParameterValid(params.get(CameraParameters::KEY_FOCUS_MODE),
        (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_FOCUS_MODES]->mPropValue))
        {
        CAMHAL_LOGDB("Focus mode set %s", params.get(CameraParameters::KEY_FOCUS_MODE));
        mParameters.set(CameraParameters::KEY_FOCUS_MODE, valstr);
        }

    if( (valstr = params.get(TICameraParameters::KEY_TOUCH_FOCUS_POS)) != NULL )
        {
        CAMHAL_LOGDB("Touch Focus position set %s", params.get(TICameraParameters::KEY_TOUCH_FOCUS_POS));
        mParameters.set(TICameraParameters::KEY_TOUCH_FOCUS_POS, valstr);
        }

    if( (valstr = params.get(TICameraParameters::KEY_FACE_DETECTION_ENABLE)) != NULL )
        {
        CAMHAL_LOGDB("Face detection set to %s", params.get(TICameraParameters::KEY_FACE_DETECTION_ENABLE));
        mParameters.set(TICameraParameters::KEY_FACE_DETECTION_ENABLE, valstr);
        }

    if( (valstr = params.get(TICameraParameters::KEY_MEASUREMENT_ENABLE)) != NULL )
        {
        CAMHAL_LOGDB("Measurements set to %s", params.get(TICameraParameters::KEY_MEASUREMENT_ENABLE));
        mParameters.set(TICameraParameters::KEY_MEASUREMENT_ENABLE, valstr);

        if (strcmp(valstr, (const char *) TICameraParameters::MEASUREMENT_ENABLE) == 0)
            {
            mMeasurementEnabled = true;
            }
        else if (strcmp(valstr, (const char *) TICameraParameters::MEASUREMENT_DISABLE) == 0)
            {
            mMeasurementEnabled = false;
            }
        else
            {
            mMeasurementEnabled = false;
            }

        }

    if( (valstr = params.get(CameraParameters::KEY_EXPOSURE_COMPENSATION)) != NULL)
        {
        CAMHAL_LOGDB("Exposure compensation set %s", params.get(CameraParameters::KEY_EXPOSURE_COMPENSATION));
        mParameters.set(CameraParameters::KEY_EXPOSURE_COMPENSATION, valstr);
        }

    if(( (valstr = params.get(CameraParameters::KEY_SCENE_MODE)) != NULL)
        && isParameterValid(params.get(CameraParameters::KEY_SCENE_MODE),
        (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_SCENE_MODES]->mPropValue))
        {
        CAMHAL_LOGDB("Scene mode set %s", params.get(CameraParameters::KEY_SCENE_MODE));
        mParameters.set(CameraParameters::KEY_SCENE_MODE, valstr);
        }

    if((params.get(CameraParameters::KEY_FLASH_MODE) != NULL)
        && isParameterValid(params.get(CameraParameters::KEY_FLASH_MODE),
        (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_FLASH_MODES]->mPropValue))
        {
        CAMHAL_LOGDB("Flash mode set %s", params.get(CameraParameters::KEY_FLASH_MODE));
        mParameters.set(CameraParameters::KEY_FLASH_MODE, params.get(CameraParameters::KEY_FLASH_MODE));
        }

    if(( (valstr = params.get(CameraParameters::KEY_EFFECT)) != NULL)
        && isParameterValid(params.get(CameraParameters::KEY_EFFECT),
        (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_EFFECTS]->mPropValue))
        {
        CAMHAL_LOGDB("Effect set %s", params.get(CameraParameters::KEY_EFFECT));
        mParameters.set(CameraParameters::KEY_EFFECT, valstr);
        }

    if(( (valstr = params.get(CameraParameters::KEY_ROTATION)) != NULL)
        && (params.getInt(CameraParameters::KEY_ROTATION) >=0))
        {
        CAMHAL_LOGDB("Rotation set %s", params.get(CameraParameters::KEY_ROTATION));
        mParameters.set(CameraParameters::KEY_ROTATION, valstr);
        }


    if(( (valstr = params.get(CameraParameters::KEY_JPEG_QUALITY)) != NULL)
        && (params.getInt(CameraParameters::KEY_JPEG_QUALITY) >=0))
        {
        CAMHAL_LOGDB("Jpeg quality set %s", params.get(CameraParameters::KEY_JPEG_QUALITY));
        mParameters.set(CameraParameters::KEY_JPEG_QUALITY, valstr);
        }

    if(( (valstr = params.get(CameraParameters::KEY_JPEG_THUMBNAIL_WIDTH)) != NULL)
        && (params.getInt(CameraParameters::KEY_JPEG_THUMBNAIL_WIDTH) >=0))
        {
        CAMHAL_LOGDB("Thumbnail width set %s", params.get(CameraParameters::KEY_JPEG_THUMBNAIL_WIDTH));
        mParameters.set(CameraParameters::KEY_JPEG_THUMBNAIL_WIDTH, valstr);
        }

    if(( (valstr = params.get(CameraParameters::KEY_JPEG_THUMBNAIL_HEIGHT)) != NULL)
        && (params.getInt(CameraParameters::KEY_JPEG_THUMBNAIL_HEIGHT) >=0))
        {
        CAMHAL_LOGDB("Thumbnail width set %s", params.get(CameraParameters::KEY_JPEG_THUMBNAIL_HEIGHT));
        mParameters.set(CameraParameters::KEY_JPEG_THUMBNAIL_HEIGHT, valstr);
        }

    if(( (valstr = params.get(CameraParameters::KEY_JPEG_THUMBNAIL_QUALITY)) != NULL )
        && (params.getInt(CameraParameters::KEY_JPEG_THUMBNAIL_QUALITY) >=0))
        {
        CAMHAL_LOGDB("Thumbnail quality set %s", params.get(CameraParameters::KEY_JPEG_THUMBNAIL_QUALITY));
        mParameters.set(CameraParameters::KEY_JPEG_THUMBNAIL_QUALITY, valstr);
        }

    if( (params.get(CameraParameters::KEY_GPS_LATITUDE) != NULL ) )
        {
        CAMHAL_LOGDB("GPS latitude set %s", params.get(CameraParameters::KEY_GPS_LATITUDE));
        mParameters.set(CameraParameters::KEY_GPS_LATITUDE, params.get(CameraParameters::KEY_GPS_LATITUDE));
        }

    if( (params.get(CameraParameters::KEY_GPS_LONGITUDE) != NULL ) )
        {
        CAMHAL_LOGDB("GPS longitude set %s", params.get(CameraParameters::KEY_GPS_LONGITUDE));
        mParameters.set(CameraParameters::KEY_GPS_LONGITUDE, params.get(CameraParameters::KEY_GPS_LONGITUDE));
        }

    if( (params.get(CameraParameters::KEY_GPS_ALTITUDE) != NULL ) )
        {
        CAMHAL_LOGDB("GPS altitude set %s", params.get(CameraParameters::KEY_GPS_ALTITUDE));
        mParameters.set(CameraParameters::KEY_GPS_ALTITUDE, params.get(CameraParameters::KEY_GPS_ALTITUDE));
        }

    if( (params.get(CameraParameters::KEY_GPS_TIMESTAMP) != NULL ) )
        {
        CAMHAL_LOGDB("GPS timestamp set %s", params.get(CameraParameters::KEY_GPS_TIMESTAMP));
        mParameters.set(CameraParameters::KEY_GPS_TIMESTAMP, params.get(CameraParameters::KEY_GPS_TIMESTAMP));
        }

    if( (params.get(CameraParameters::KEY_GPS_PROCESSING_METHOD) != NULL ) )
        {
        CAMHAL_LOGDB("GPS processing method set %s", params.get(CameraParameters::KEY_GPS_PROCESSING_METHOD));
        mParameters.set(CameraParameters::KEY_GPS_PROCESSING_METHOD, params.get(CameraParameters::KEY_GPS_PROCESSING_METHOD));
        }

    if( (params.get(TICameraParameters::KEY_GPS_ALTITUDE_REF) != NULL ) )
        {
        CAMHAL_LOGDB("GPS altitude ref set %s", params.get(TICameraParameters::KEY_GPS_ALTITUDE_REF));
        mParameters.set(TICameraParameters::KEY_GPS_ALTITUDE_REF, params.get(TICameraParameters::KEY_GPS_ALTITUDE_REF));
        }

    if( (params.get(TICameraParameters::KEY_GPS_MAPDATUM ) != NULL ) )
        {
        CAMHAL_LOGDB("GPS MAPDATUM set %s", params.get(TICameraParameters::KEY_GPS_MAPDATUM));
        mParameters.set(TICameraParameters::KEY_GPS_MAPDATUM, params.get(TICameraParameters::KEY_GPS_MAPDATUM));
        }

    if( (params.get(TICameraParameters::KEY_GPS_VERSION ) != NULL ) )
        {
        CAMHAL_LOGDB("GPS MAPDATUM set %s", params.get(TICameraParameters::KEY_GPS_VERSION));
        mParameters.set(TICameraParameters::KEY_GPS_VERSION, params.get(TICameraParameters::KEY_GPS_VERSION));
        }

    if( (params.get(TICameraParameters::KEY_EXP_BRACKETING_RANGE ) != NULL ) )
        {
        CAMHAL_LOGDB("Exposure Bracketing set %s", params.get(TICameraParameters::KEY_EXP_BRACKETING_RANGE));
        mParameters.set(TICameraParameters::KEY_EXP_BRACKETING_RANGE, params.get(TICameraParameters::KEY_EXP_BRACKETING_RANGE));
        }

    if( ( (valstr = params.get(CameraParameters::KEY_ZOOM)) != NULL )
        && (params.getInt(CameraParameters::KEY_ZOOM) >= 0 )
        && (params.getInt(CameraParameters::KEY_ZOOM) <= mMaxZoomSupported ) )
        {
        CAMHAL_LOGDB("Zoom set %s", params.get(CameraParameters::KEY_ZOOM));
        mParameters.set(CameraParameters::KEY_ZOOM, valstr);
        }
    else
        {
        ret = -EINVAL;
        }

    if ( NULL != mCameraAdapter )
        {
        ret |= mCameraAdapter->setParameters(mParameters);
        }

    if( NULL != params.get(TICameraParameters::KEY_TEMP_BRACKETING_RANGE_POS) )
        {
        int posBracketRange = params.getInt(TICameraParameters::KEY_TEMP_BRACKETING_RANGE_POS);
        if ( 0 < posBracketRange )
            {
            mBracketRangePositive = posBracketRange;
            }
        }
    CAMHAL_LOGEB("Positive bracketing range %d", mBracketRangePositive);


    if( NULL != params.get(TICameraParameters::KEY_TEMP_BRACKETING_RANGE_NEG) )
        {
        int negBracketRange = params.getInt(TICameraParameters::KEY_TEMP_BRACKETING_RANGE_NEG);
        if ( 0 < negBracketRange )
            {
            mBracketRangeNegative = negBracketRange;
            }
        }
    CAMHAL_LOGEB("Negative bracketing range %d", mBracketRangeNegative);

    if( ( (valstr = params.get(TICameraParameters::KEY_TEMP_BRACKETING)) != NULL) &&
        ( strcmp(valstr, TICameraParameters::BRACKET_ENABLE) == 0 ))
        {
        if ( !mBracketingEnabled )
            {
            CAMHAL_LOGDA("Enabling bracketing");
            mBracketingEnabled = true;

            //Wait for AF events to enable bracketing
            if ( NULL != mCameraAdapter )
                {
                setEventProvider( CameraHalEvent::ALL_EVENTS, mCameraAdapter );
                }
            }
        else
            {
            CAMHAL_LOGDA("Bracketing already enabled");
            }
        }
    else if ( ( (valstr = params.get(TICameraParameters::KEY_TEMP_BRACKETING)) != NULL ) &&
        ( strcmp(valstr, TICameraParameters::BRACKET_DISABLE) == 0 ))
        {
        CAMHAL_LOGDA("Disabling bracketing");

        mBracketingEnabled = false;
        stopImageBracketing();

        //Remove AF events subscription
        if ( NULL != mEventProvider )
            {
            mEventProvider->disableEventNotification( CameraHalEvent::ALL_EVENTS );
            delete mEventProvider;
            mEventProvider = NULL;
            }

        }

    if( ( (valstr = params.get(TICameraParameters::KEY_SHUTTER_ENABLE)) != NULL ) &&
        ( strcmp(valstr, TICameraParameters::SHUTTER_ENABLE) == 0 ))
        {
        CAMHAL_LOGDA("Enabling shutter sound");

        mShutterEnabled = true;
        mMsgEnabled |= CAMERA_MSG_SHUTTER;
        mParameters.set(TICameraParameters::KEY_SHUTTER_ENABLE, valstr);
        }
    else if ( ( (valstr = params.get(TICameraParameters::KEY_SHUTTER_ENABLE)) != NULL ) &&
        ( strcmp(valstr, TICameraParameters::SHUTTER_DISABLE) == 0 ))
        {
        CAMHAL_LOGDA("Disabling shutter sound");

        mShutterEnabled = false;
        mMsgEnabled &= ~CAMERA_MSG_SHUTTER;
        mParameters.set(TICameraParameters::KEY_SHUTTER_ENABLE, valstr);
        }


    CAMHAL_LOGDB("mReloadAdapter %d", (int) mReloadAdapter);

    LOG_FUNCTION_NAME_EXIT

    return ret;

    }

status_t CameraHal::allocPreviewBufs(int width, int height, const char* previewFormat)
{
    status_t ret;
    BufferProvider* newBufProvider;

    LOG_FUNCTION_NAME

    /**
      * Logic for allocation of preview buffers : If an overlay object is available, we allocate the preview buffers using
      * that. If not, then we use Mem Manager to allocate the buffers
      */

    ///If overlay is initialized, allocate buffer using overlay
    if(mDisplayAdapter.get() != NULL)
        {
        CAMHAL_LOGDA("DisplayAdapter present. Choosing DisplayAdapter as buffer allocator");
        newBufProvider = (BufferProvider*) mDisplayAdapter.get();
        mPreviewBufsAllocatedUsingOverlay = true;
        }
    else
        {
        CAMHAL_LOGDA("DisplayAdapter not present. Choosing MemoryManager as buffer allocator");
        newBufProvider = (BufferProvider*) mMemoryManager.get();
        mPreviewBufsAllocatedUsingOverlay = false;
        }

    if(!mPreviewBufs)
        {

        ///@todo Pluralise the name of this method to allocateBuffers
        mPreviewLength = 0;
        mPreviewBufs = (int32_t *) newBufProvider->allocateBuffer(width, height, previewFormat, mPreviewLength, atoi(mCameraPropertiesArr[CameraProperties::PROP_INDEX_REQUIRED_PREVIEW_BUFS]->mPropValue));
        mPreviewOffsets = (uint32_t *) newBufProvider->getOffsets();
        mPreviewFd = newBufProvider->getFd();
        mBufProvider = newBufProvider;

        }
    else
        {
            ///If the existing buffer provider is not DisplayAdapter but the new one is, free the buffers allocated using previous buffer provider
            ///and allocate buffers using the DisplayAdapter (which in turn will query the buffers from Overlay object and just pass the pointers)
            ///@todo currently having a static value for number of buffers, this may need to be made dynamic/flexible  b/w preview and video modes
            if((mBufProvider!=(BufferProvider*)mDisplayAdapter.get()) && (newBufProvider==(BufferProvider*)mDisplayAdapter.get()))
                {

                freePreviewBufs();

                mPreviewLength = 0;
                mPreviewBufs = (int32_t *)newBufProvider->allocateBuffer(width, height, previewFormat, mPreviewLength, atoi(mCameraPropertiesArr[CameraProperties::PROP_INDEX_REQUIRED_PREVIEW_BUFS]->mPropValue));
                if ( NULL == mPreviewBufs )
                    {
                    CAMHAL_LOGEA("Couldn't allocate preview buffers using Memory manager");
                    LOG_FUNCTION_NAME_EXIT
                    return NO_MEMORY;
                    }

                mPreviewOffsets = (uint32_t *) newBufProvider->getOffsets();
                mPreviewFd = newBufProvider->getFd();
                mBufProvider = newBufProvider;

                }
        }

    LOG_FUNCTION_NAME_EXIT

    return NO_ERROR;

}

status_t CameraHal::freePreviewBufs()
{
    status_t ret = NO_ERROR;
    LOG_FUNCTION_NAME

    CAMHAL_LOGDB("mPreviewBufs = 0x%x", (unsigned int)mPreviewBufs);
    if(mPreviewBufs)
        {
        ///@todo Pluralise the name of this method to freeBuffers
        ret = mBufProvider->freeBuffer(mPreviewBufs);
        mPreviewBufs = NULL;
        LOG_FUNCTION_NAME_EXIT
        return ret;
        }
    LOG_FUNCTION_NAME_EXIT;
    return ret;
}


status_t CameraHal::allocPreviewDataBufs(size_t size, size_t bufferCount)
{
    status_t ret = NO_ERROR;
    int bytes;

    LOG_FUNCTION_NAME

    bytes = size;

    if ( NO_ERROR == ret )
        {
        if( NULL != mPreviewDataBufs )
            {
            ret = freePreviewDataBufs();
            }
        }

    if ( NO_ERROR == ret )
        {
        mPreviewDataBufs = (int32_t *)mMemoryManager->allocateBuffer(0, 0, NULL, bytes, bufferCount);

        CAMHAL_LOGEB("Size of Preview data buffer = %d", bytes);
        if( NULL == mPreviewDataBufs )
            {
            CAMHAL_LOGEA("Couldn't allocate image buffers using memory manager");
            ret = -NO_MEMORY;
            }
        else
            {
            bytes = size;
            }
        }

    if ( NO_ERROR == ret )
        {
        mPreviewDataFd = mMemoryManager->getFd();
        mPreviewDataLength = bytes;
        mPreviewDataOffsets = mMemoryManager->getOffsets();
        }
    else
        {
        mPreviewDataFd = -1;
        mPreviewDataLength = 0;
        mPreviewDataOffsets = NULL;
        }

    LOG_FUNCTION_NAME

    return ret;
}

status_t CameraHal::freePreviewDataBufs()
{
    status_t ret = NO_ERROR;

    LOG_FUNCTION_NAME

    if ( NO_ERROR == ret )
        {

        if( NULL != mPreviewDataBufs )
            {

            ///@todo Pluralise the name of this method to freeBuffers
            ret = mMemoryManager->freeBuffer(mPreviewDataBufs);
            mPreviewDataBufs = NULL;

            }
        else
            {
            ret = -EINVAL;
            }

        }

    LOG_FUNCTION_NAME_EXIT

    return ret;
}

status_t CameraHal::allocImageBufs(unsigned int width, unsigned int height, size_t size, const char* previewFormat, unsigned int bufferCount)
{
    status_t ret = NO_ERROR;
    int bytes;

    LOG_FUNCTION_NAME

    bytes = size;

    ///@todo Enhance this method allocImageBufs() to take in a flag for burst capture
    ///Always allocate the buffers for image capture using MemoryManager
    if ( NO_ERROR == ret )
        {
        if( ( NULL != mImageBufs ) )
            {
            ret = freeImageBufs();
            }
        }

    if ( NO_ERROR == ret )
        {
        mImageBufs = (int32_t *)mMemoryManager->allocateBuffer(0, 0, previewFormat, bytes, bufferCount);

        CAMHAL_LOGEB("Size of Image cap buffer = %d", bytes);
        if( NULL == mImageBufs )
            {
            CAMHAL_LOGEA("Couldn't allocate image buffers using memory manager");
            ret = -NO_MEMORY;
            }
        else
            {
            bytes = size;
            }
        }

    if ( NO_ERROR == ret )
        {
        mImageFd = mMemoryManager->getFd();
        mImageLength = bytes;
        mImageOffsets = mMemoryManager->getOffsets();
        }
    else
        {
        mImageFd = -1;
        mImageLength = 0;
        mImageOffsets = NULL;
        }

    LOG_FUNCTION_NAME

    return ret;
}

void endImageCapture( void *userData)
{
    LOG_FUNCTION_NAME

    if ( NULL != userData )
        {
        CameraHal *c = reinterpret_cast<CameraHal *>(userData);
        c->signalEndImageCapture();
        }

    LOG_FUNCTION_NAME_EXIT
}

void releaseImageBuffers(void *userData)
{
    LOG_FUNCTION_NAME

    if ( NULL != userData )
        {
        CameraHal *c = reinterpret_cast<CameraHal *>(userData);
        c->freeImageBufs();
        }

    LOG_FUNCTION_NAME_EXIT
}

status_t CameraHal::signalEndImageCapture()
{
    status_t ret = NO_ERROR;
    bool restartImageCapture = false;

    LOG_FUNCTION_NAME

    if ( NO_ERROR == ret )
        {
        Mutex::Autolock lock(mLock);

        mImageCaptureRunning = false;

        if ( 0 < mTakePictureQueue )
            {
            mTakePictureQueue--;
            restartImageCapture = true;
            }

        }

    if ( restartImageCapture )
        {
        ret = takePicture();
        }

    LOG_FUNCTION_NAME_EXIT

    return ret;
}

status_t CameraHal::freeImageBufs()
{
    status_t ret = NO_ERROR;

    LOG_FUNCTION_NAME

    if ( NO_ERROR == ret )
        {

        if( NULL != mImageBufs )
            {

            ///@todo Pluralise the name of this method to freeBuffers
            ret = mMemoryManager->freeBuffer(mImageBufs);
            mImageBufs = NULL;

            }
        else
            {
            ret = -EINVAL;
            }

        }

    LOG_FUNCTION_NAME_EXIT

    return ret;
}



/**
   @brief Start preview mode.

   @param none
   @return NO_ERROR Camera switched to VF mode
   @todo Update function header with the different errors that are possible

 */
status_t CameraHal::startPreview()
{

    status_t ret = NO_ERROR;
    int w, h;
    size_t frameDataSize;
    const char *valstr = NULL;

#if PPM_INSTRUMENTATION || PPM_INSTRUMENTATION_ABS

        gettimeofday(&mStartPreview, NULL);

#endif


    LOG_FUNCTION_NAME

    if( (mDisplayAdapter.get() != NULL) && ( !mPreviewEnabled ) && ( mDisplayPaused ) )
        {
        CAMHAL_LOGDA("Preview is in paused state");

        mDisplayPaused = false;
        mPreviewEnabled = true;
        if ( NO_ERROR == ret )
            {
            ret = mDisplayAdapter->pauseDisplay(mDisplayPaused);

            if ( NO_ERROR != ret )
                {
                CAMHAL_LOGEB("Display adapter resume failed %x", ret);
                }
            }

        return ret;

        }
    else if ( mPreviewEnabled )
        {
        CAMHAL_LOGEA("Preview already running");

        LOG_FUNCTION_NAME_EXIT

        return ALREADY_EXISTS;
        }

    ///If we don't have the preview callback enabled and display adapter,
    ///@todo we dont support changing the overlay dynamically when preview is ongoing nor set the overlay object dynamically
    ///@remarks We support preview without overlay. setPreviewDisplay() should be passed with NULL overlay for this path
    ///                 to work
    if(//!(mMsgEnabled & CAMERA_MSG_PREVIEW_FRAME) &&
         (!mSetOverlayCalled))
        {
        CAMHAL_LOGEA("Preview not started. Preview in progress flag set");
        mPreviewStartInProgress = true;
        return NO_ERROR;
        }

    /// Ensure that buffers for preview are allocated before we start the camera
    ///Get the updated size from Camera Adapter, to account for padding etc
    mCameraAdapter->getFrameSize(w, h);

    ///Update the current preview width and height
    mPreviewWidth = w;
    mPreviewHeight = h;

    //Update the padded width and height - required for VNF and VSTAB
    mParameters.set(TICameraParameters::KEY_PADDED_WIDTH, mPreviewWidth);
    mParameters.set(TICameraParameters::KEY_PADDED_HEIGHT, mPreviewHeight);

    ///Allocate the preview buffers
    ret = allocPreviewBufs(w, h, mParameters.getPreviewFormat());
    if ( NO_ERROR != ret )
        {
        CAMHAL_LOGEA("Couldn't allocate buffers for Preview");
        goto error;
        }

    if ( mMeasurementEnabled )
        {

        ret = mCameraAdapter->getFrameDataSize(frameDataSize, atoi(mCameraPropertiesArr[CameraProperties::PROP_INDEX_REQUIRED_PREVIEW_BUFS]->mPropValue));

        if ( NO_ERROR == ret )
            {
            ///Allocate the preview data buffers
            ret = allocPreviewDataBufs(frameDataSize, atoi(mCameraPropertiesArr[CameraProperties::PROP_INDEX_REQUIRED_PREVIEW_BUFS]->mPropValue));
            if ( NO_ERROR != ret )
                {
                CAMHAL_LOGEA("Couldn't allocate preview data buffers");
                goto error;
                }
            }

        if ( NO_ERROR == ret )
            {
            mCameraAdapter->useBuffers(CameraAdapter::CAMERA_MEASUREMENT, mPreviewDataBufs, mPreviewDataOffsets, mPreviewDataFd, mPreviewDataLength, atoi(mCameraPropertiesArr[CameraProperties::PROP_INDEX_REQUIRED_PREVIEW_BUFS]->mPropValue));
            }

        }

    ///Pass the buffers to Camera Adapter
    mCameraAdapter->useBuffers(CameraAdapter::CAMERA_PREVIEW, mPreviewBufs, mPreviewOffsets, mPreviewFd, mPreviewLength, atoi(mCameraPropertiesArr[CameraProperties::PROP_INDEX_REQUIRED_PREVIEW_BUFS]->mPropValue));

    mAppCallbackNotifier->startPreviewCallbacks(mParameters, mPreviewBufs, mPreviewOffsets, mPreviewFd, mPreviewLength, atoi(mCameraPropertiesArr[CameraProperties::PROP_INDEX_REQUIRED_PREVIEW_BUFS]->mPropValue));

    ///Start the callback notifier
    ret = mAppCallbackNotifier->start();

    if(ret!=NO_ERROR)
        {
        CAMHAL_LOGEA("Couldn't start AppCallbackNotifier");
        goto error;
        }

    CAMHAL_LOGDA("Started AppCallbackNotifier..");


    if ( NO_ERROR == ret )
        {
        mAppCallbackNotifier->setMeasurements(mMeasurementEnabled);
        }

    ///Enable the display adapter if present, actual overlay enable happens when we post the buffer
    if(mDisplayAdapter.get() != NULL)
        {
        CAMHAL_LOGDA("Enabling display");
        bool isS3d = false;
        if ( (valstr = mParameters.get(TICameraParameters::KEY_S3D_SUPPORTED)) != NULL) {
            isS3d = (strcmp(valstr, "true") == 0);
        }
        DisplayAdapter::S3DParameters s3dParams;
        if ( (valstr = mParameters.get(TICameraParameters::KEY_S3D2D_PREVIEW)) != NULL) {
            if (strcmp(valstr, "off") == 0)
                {
                CAMHAL_LOGEA("STEREO 3D->2D PREVIEW MODE IS OFF");
                //TODO: obtain the frame packing configuration from camera or user settings
                //once side by side configuration is supported
                s3dParams.mode = OVERLAY_S3D_MODE_ON;
                s3dParams.framePacking = OVERLAY_S3D_FORMAT_OVERUNDER;
                s3dParams.order = OVERLAY_S3D_ORDER_LF;
                s3dParams.subSampling = OVERLAY_S3D_SS_NONE;
                }
            else
                {
                CAMHAL_LOGEA("STEREO 3D->2D PREVIEW MODE IS ON");
                s3dParams.mode = OVERLAY_S3D_MODE_OFF;
                s3dParams.framePacking = OVERLAY_S3D_FORMAT_OVERUNDER;
                s3dParams.order = OVERLAY_S3D_ORDER_LF;
                s3dParams.subSampling = OVERLAY_S3D_SS_NONE;
                }
            }

#if PPM_INSTRUMENTATION || PPM_INSTRUMENTATION_ABS

        ret = mDisplayAdapter->enableDisplay(&mStartPreview, isS3d ? &s3dParams : NULL);

#else

        ret = mDisplayAdapter->enableDisplay(NULL, isS3d ? &s3dParams : NULL);

#endif

        if ( ret != NO_ERROR )
            {
            CAMHAL_LOGEA("Couldn't enable display");
            goto error;
            }

        }

    ///Send START_PREVIEW command to adapter
    CAMHAL_LOGDA("Starting CameraAdapter preview mode");

    ret = mCameraAdapter->sendCommand(CameraAdapter::CAMERA_START_PREVIEW);

    if(ret!=NO_ERROR)
        {
        CAMHAL_LOGEA("Couldn't start preview w/ CameraAdapter");
        goto error;
        }
    CAMHAL_LOGDA("Started preview");


    mPreviewEnabled = true;
    mPreviewStartInProgress = false;
    return ret;

    error:

        CAMHAL_LOGEA("Performing cleanup after error");

        //Do all the cleanup
        freePreviewBufs();
        mCameraAdapter->sendCommand(CameraAdapter::CAMERA_STOP_PREVIEW);
        if(mDisplayAdapter.get() != NULL)
            {
            mDisplayAdapter->disableDisplay();
            }
        mAppCallbackNotifier->stop();
        mPreviewStartInProgress = false;
        mPreviewEnabled = false;
        LOG_FUNCTION_NAME_EXIT

        return ret;
}

/**
   @brief Configures an overlay object to use for direct rendering

   A NULL overlay object will disable direct rendering. In such cases, the application / camera service
   will take care of rendering to the screen.

   @param[in] overlay The overlay object created by Surface flinger, and talks to V4L2 driver for render
   @return NO_ERROR If the overlay object passes validation criteria
   @todo Define validation criteria for overlay object. Define error codes for scenarios

 */
status_t CameraHal::setOverlay(const sp<Overlay> &overlay)
{
    status_t ret = NO_ERROR;
    LOG_FUNCTION_NAME

    mSetOverlayCalled = true;


   ///If the Camera service passes a null overlay, we destroy existing overlay and free the DisplayAdapter
    if(!overlay.get())
        {
        if(mDisplayAdapter.get() != NULL)
            {
            ///NULL overlay passed, destroy the display adapter if present
            CAMHAL_LOGEA("NULL Overlay passed to setOverlay, destroying display adapter");
            mDisplayAdapter.clear();
            ///@remarks If there was an overlay previously existing, we usually expect another valid overlay to be passed by the client
            ///@remarks so, we will wait until it passes a valid overlay to begin the preview again
            mSetOverlayCalled = false;
            }
        CAMHAL_LOGEA("NULL Overlay passed to setOverlay");
        // Setting the buffer provider to memory manager
        mBufProvider = (BufferProvider*) mMemoryManager.get();
        return ret;
        }

    ///Destroy existing display adapter, if present
    mDisplayAdapter.clear();


    ///Need to create the display adapter since it has not been created
    /// Create display adapter
    mDisplayAdapter = new OverlayDisplayAdapter();
    ret = NO_ERROR;
    if(!mDisplayAdapter.get() || ((ret=mDisplayAdapter->initialize())!=NO_ERROR))
        {
        if(ret!=NO_ERROR)
            {
            mDisplayAdapter.clear();
            CAMHAL_LOGEA("DisplayAdapter initialize failed")
            LOG_FUNCTION_NAME_EXIT
            return ret;
            }
        else
            {
            CAMHAL_LOGEA("Couldn't create DisplayAdapter");
            LOG_FUNCTION_NAME_EXIT
            return NO_MEMORY;
            }
        }

    ///DisplayAdapter needs to know where to get the CameraFrames from inorder to display
    ///Since CameraAdapter is the one that provides the frames, set it as the frame provider for DisplayAdapter
    mDisplayAdapter->setFrameProvider(mCameraAdapter);

    ///Any dynamic errors that happen during the camera use case has to be propagated back to the application
    ///via CAMERA_MSG_ERROR. AppCallbackNotifier is the class that  notifies such errors to the application
    ///Set it as the error handler for the DisplayAdapter
    mDisplayAdapter->setErrorHandler(mAppCallbackNotifier.get());


    ///Update the display adapter with the new overlay that is passed from CameraService
    ret  = mDisplayAdapter->setOverlay(overlay);
    if(ret!=NO_ERROR)
        {
        CAMHAL_LOGEB("DisplayAdapter setOverlay returned error %d", ret);
        }


    ///If setOverlay was called when preview is already running, then ask CameraAdapter to flush the buffers
    ///and then use the new set of buffers from Overlay
    ///Note that this doesn't require camera to be restarted. But if it is a V4L2 type of interface, it might have to
    ///stop and re-start the camera
    ///@todo In the future, need to extend overlay to use buffers allocated in CameraHal so that we do not
    ///       have to
    if(mPreviewEnabled)
        {
        CAMHAL_LOGDA("setOverlay called when preview running - Handling buffers for overlay");
        if(!mDisplayAdapter->supportsExternalBuffering())
            {
            ///CameraAdapter is expected to relinquish all buffers when this call is made
            mCameraAdapter->flushBuffers();

            int w, h;
            ///Allocate preview buffers using overlay
            mParameters.getPreviewSize(&w, &h);

            ///Allocate buffers
            ret = allocPreviewBufs(w, h, mParameters.getPreviewFormat());
            if(ret!=NO_ERROR)
                {
                CAMHAL_LOGEA("Couldn't allocate buffers for Preview using overlay");
                return ret;
                }

            ///Pass the buffers to Camera Adapter
            mCameraAdapter->useBuffers(CameraAdapter::CAMERA_PREVIEW, mPreviewBufs, mPreviewOffsets, mPreviewFd, mPreviewLength, atoi(mCameraPropertiesArr[CameraProperties::PROP_INDEX_REQUIRED_PREVIEW_BUFS]->mPropValue));

            }
        else
            {
            ///Overlay supports external buffering, let DisplayAdapter know about the Camera buffers
            mDisplayAdapter->useBuffers(mPreviewBufs, atoi(mCameraPropertiesArr[CameraProperties::PROP_INDEX_REQUIRED_PREVIEW_BUFS]->mPropValue));
            }

        ///Everything initialized, It's time to start the display of frames through overlay
        ret = mDisplayAdapter->enableDisplay();
        if(ret!=NO_ERROR)
            {
            CAMHAL_LOGEA("Couldn't enable display");
            return ret;
            }

        }
    else if(mPreviewStartInProgress)
        {
        ///Start the preview since the overlay is now available
        ret = startPreview();
        }

    LOG_FUNCTION_NAME_EXIT

    return ret;

}


/**
   @brief Stop a previously started preview.

   @param none
   @return none

 */
void CameraHal::stopPreview()
{
    LOG_FUNCTION_NAME

    if(!previewEnabled() && !mDisplayPaused)
        {
        LOG_FUNCTION_NAME_EXIT
        return;
        }

    if(mDisplayAdapter.get() != NULL)
        {
        ///Stop the buffer display first
        mDisplayAdapter->disableDisplay();
        }



    if(mAppCallbackNotifier.get() != NULL)
        {

        //Stop the callback sending
        mAppCallbackNotifier->stop();


        mAppCallbackNotifier->stopPreviewCallbacks();
        }


    if ( NULL != mCameraAdapter )
       {
        //Stop the source of frames
        mCameraAdapter->sendCommand(CameraAdapter::CAMERA_STOP_PREVIEW);
        }

    freePreviewBufs();
    freePreviewDataBufs();


    mPreviewEnabled = false;
    mDisplayPaused = false;

    LOG_FUNCTION_NAME_EXIT
}

/**
   @brief Returns true if preview is enabled

   @param none
   @return true If preview is running currently
         false If preview has been stopped

 */
bool CameraHal::previewEnabled()
{
    LOG_FUNCTION_NAME

    return (mPreviewEnabled || mPreviewStartInProgress);
}

/**
   @brief Start record mode.

  When a record image is available a CAMERA_MSG_VIDEO_FRAME message is sent with
  the corresponding frame. Every record frame must be released by calling
  releaseRecordingFrame().

   @param none
   @return NO_ERROR If recording could be started without any issues
   @todo Update the header with possible error values in failure scenarios

 */
status_t CameraHal::startRecording( )
{
    int w, h;
    status_t ret = NO_ERROR;

    LOG_FUNCTION_NAME


#if PPM_INSTRUMENTATION || PPM_INSTRUMENTATION_ABS

            gettimeofday(&mStartPreview, NULL);

#endif

    if(!previewEnabled())
        {
        return NO_INIT;
        }

    if ( NO_ERROR == ret )
        {
         ret = mAppCallbackNotifier->initSharedVideoBuffers(mPreviewBufs, mPreviewOffsets, mPreviewFd, mPreviewLength, atoi(mCameraPropertiesArr[CameraProperties::PROP_INDEX_REQUIRED_PREVIEW_BUFS]->mPropValue));
        }

    if ( NO_ERROR == ret )
        {
         ret = mAppCallbackNotifier->startRecording();
        }

    if ( NO_ERROR == ret )
        {
        ///Buffers for video capture (if different from preview) are expected to be allocated within CameraAdapter
         ret =  mCameraAdapter->sendCommand(CameraAdapter::CAMERA_START_VIDEO);
        }

    if ( NO_ERROR == ret )
        {
        mRecordingEnabled = true;
        }

    LOG_FUNCTION_NAME_EXIT

    return ret;
}

/**
   @brief Stop a previously started recording.

   @param none
   @return none

 */
void CameraHal::stopRecording()
{
    LOG_FUNCTION_NAME

    if (!mRecordingEnabled )
        {
        return;
        }

    mAppCallbackNotifier->stopRecording();

    mCameraAdapter->sendCommand(CameraAdapter::CAMERA_STOP_VIDEO);

    mRecordingEnabled = false;

    LOG_FUNCTION_NAME_EXIT
}

/**
   @brief Returns true if recording is enabled.

   @param none
   @return true If recording is currently running
         false If recording has been stopped

 */
bool CameraHal::recordingEnabled()
{
    LOG_FUNCTION_NAME

    LOG_FUNCTION_NAME_EXIT

    return mRecordingEnabled;
}

/**
   @brief Release a record frame previously returned by CAMERA_MSG_VIDEO_FRAME.

   @param[in] mem MemoryBase pointer to the frame being released. Must be one of the buffers
               previously given by CameraHal
   @return none

 */
void CameraHal::releaseRecordingFrame(const sp<IMemory>& mem)
{
    LOG_FUNCTION_NAME

    //CAMHAL_LOGDB(" 0x%x", mem->pointer());

    if ( ( mRecordingEnabled ) && ( NULL != mem.get() ) )
        {
        mAppCallbackNotifier->releaseRecordingFrame(mem);
        }

    LOG_FUNCTION_NAME_EXIT

    return;
}

/**
   @brief Start auto focus

   This call asynchronous.
   The notification callback routine is called with CAMERA_MSG_FOCUS once when
   focusing is complete. autoFocus() will be called again if another auto focus is
   needed.

   @param none
   @return NO_ERROR
   @todo Define the error codes if the focus is not locked

 */
status_t CameraHal::autoFocus()
{
    status_t ret = NO_ERROR;

#if PPM_INSTRUMENTATION || PPM_INSTRUMENTATION_ABS

    gettimeofday(&mStartFocus, NULL);

#endif


    LOG_FUNCTION_NAME

    if ( NULL != mCameraAdapter )
        {

#if PPM_INSTRUMENTATION || PPM_INSTRUMENTATION_ABS

    //pass the autoFocus timestamp along with the command to camera adapter
    ret = mCameraAdapter->sendCommand(CameraAdapter::CAMERA_PERFORM_AUTOFOCUS, ( int ) &mStartFocus);

#else

    ret = mCameraAdapter->sendCommand(CameraAdapter::CAMERA_PERFORM_AUTOFOCUS);

#endif

        }
    else
        {
            ret = -1;
        }

    LOG_FUNCTION_NAME_EXIT

    return ret;
}

/**
   @brief Cancels auto-focus function.

   If the auto-focus is still in progress, this function will cancel it.
   Whether the auto-focus is in progress or not, this function will return the
   focus position to the default. If the camera does not support auto-focus, this is a no-op.


   @param none
   @return NO_ERROR If the cancel succeeded
   @todo Define error codes if cancel didnt succeed

 */
status_t CameraHal::cancelAutoFocus()
{
    LOG_FUNCTION_NAME
    ///@todo Implement this when auto focus will be supported
    return NO_ERROR;
}

void CameraHal::setEventProvider(int32_t eventMask, MessageNotifier * eventNotifier)
{

    LOG_FUNCTION_NAME

    if ( NULL != mEventProvider )
        {
        mEventProvider->disableEventNotification(CameraHalEvent::ALL_EVENTS);
        delete mEventProvider;
        mEventProvider = NULL;
        }

    mEventProvider = new EventProvider(eventNotifier, this, eventCallbackRelay);
    if ( NULL == mEventProvider )
        {
        CAMHAL_LOGEA("Error in creating EventProvider");
        }
    else
        {
        mEventProvider->enableEventNotification(eventMask);
        }

    LOG_FUNCTION_NAME_EXIT
}

void CameraHal::eventCallbackRelay(CameraHalEvent* event)
{
    LOG_FUNCTION_NAME

    CameraHal *appcbn = ( CameraHal * ) (event->mCookie);
    appcbn->eventCallback(event );

    LOG_FUNCTION_NAME_EXIT
}

void CameraHal::eventCallback(CameraHalEvent* event)
{
    LOG_FUNCTION_NAME

    if ( NULL != event )
        {
        switch( event->mEventType )
            {
            case CameraHalEvent::EVENT_FOCUS_LOCKED:
            case CameraHalEvent::EVENT_FOCUS_ERROR:
                {
                if ( mBracketingEnabled )
                    {
                    startImageBracketing();
                    }
                break;
                }
            default:
                {
                break;
                }
            };
        }

    LOG_FUNCTION_NAME_EXIT
}

status_t CameraHal::startImageBracketing()
{
        status_t ret = NO_ERROR;
        int width, height;
        size_t pictureBufferLength;

#if PPM_INSTRUMENTATION || PPM_INSTRUMENTATION_ABS

        gettimeofday(&mStartCapture, NULL);

#endif

        LOG_FUNCTION_NAME

        if(!previewEnabled() && !mDisplayPaused)
            {
            LOG_FUNCTION_NAME_EXIT
            return NO_INIT;
            }

        if ( !mBracketingEnabled )
            {
            return ret;
            }

        if ( NO_ERROR == ret )
            {
            mBracketingRunning = true;
            }

        if (  (NO_ERROR == ret) && ( NULL != mCameraAdapter ) )
            {
            ret = mCameraAdapter->getPictureBufferSize(pictureBufferLength, ( mBracketRangeNegative + 1 ));

            if ( NO_ERROR != ret )
                {
                CAMHAL_LOGEB("getPictureBufferSize returned error 0x%x", ret);
                }
            }

        if ( NO_ERROR == ret )
            {
            mParameters.getPictureSize(&width, &height);

            ret = allocImageBufs(width, height, pictureBufferLength, mParameters.getPictureFormat(), ( mBracketRangeNegative + 1 ));
            if ( NO_ERROR != ret )
                {
                CAMHAL_LOGEB("allocImageBufs returned error 0x%x", ret);
                }
            }

        if (  (NO_ERROR == ret) && ( NULL != mCameraAdapter ) )
            {

            ret = mCameraAdapter->useBuffers(CameraAdapter::CAMERA_IMAGE_CAPTURE, mImageBufs, mImageOffsets, mImageFd, mImageLength, ( mBracketRangeNegative + 1 ));

            if ( NO_ERROR == ret )
                {

#if PPM_INSTRUMENTATION || PPM_INSTRUMENTATION_ABS

                 //pass capture timestamp along with the camera adapter command
                ret = mCameraAdapter->sendCommand(CameraAdapter::CAMERA_START_BRACKET_CAPTURE,  ( mBracketRangePositive + 1 ),  (int) &mStartCapture);

#else

                ret = mCameraAdapter->sendCommand(CameraAdapter::CAMERA_START_BRACKET_CAPTURE, ( mBracketRangePositive + 1 ));

#endif

                }
            }

        return ret;
}

status_t CameraHal::stopImageBracketing()
{
        status_t ret = NO_ERROR;

        LOG_FUNCTION_NAME

        if ( !mBracketingRunning )
            {
            return ret;
            }

        if ( NO_ERROR == ret )
            {
            mBracketingRunning = false;
            }

        if(!previewEnabled() && !mDisplayPaused)
            {
            return NO_INIT;
            }

        if ( NO_ERROR == ret )
            {
            ret = mCameraAdapter->sendCommand(CameraAdapter::CAMERA_STOP_BRACKET_CAPTURE);
            }

        LOG_FUNCTION_NAME_EXIT

        return ret;
}

/**
   @brief Take a picture.

   @param none
   @return NO_ERROR If able to switch to image capture
   @todo Define error codes if unable to switch to image capture

 */
status_t CameraHal::takePicture( )
{
    status_t ret = NO_ERROR;
    int width, height;
    size_t pictureBufferLength;
    int burst;
    unsigned int bufferCount = 1;

    Mutex::Autolock lock(mLock);

#if PPM_INSTRUMENTATION || PPM_INSTRUMENTATION_ABS

    gettimeofday(&mStartCapture, NULL);

#endif

    LOG_FUNCTION_NAME

    if(!previewEnabled() && !mDisplayPaused)
        {
        LOG_FUNCTION_NAME_EXIT
        return NO_INIT;
        }

    //If capture has already started, then queue this call for later execution
    if ( mImageCaptureRunning )
        {
        mTakePictureQueue++;
        return ret;
        }
    else
        {
        mImageCaptureRunning = true;
        }

    if ( !mBracketingRunning )
        {

         if ( NO_ERROR == ret )
            {
            burst = mParameters.getInt(TICameraParameters::KEY_BURST);
            }

         //Allocate all buffers only in burst capture case
         if ( burst > 1 )
             {
             bufferCount = CameraHal::NO_BUFFERS_IMAGE_CAPTURE;
             }

        //Pause Preview during capture
        if ( (NO_ERROR == ret) && ( NULL != mDisplayAdapter.get() ) && ( burst < 1 ) )
            {
            mDisplayPaused = true;
            mPreviewEnabled = false;
            ret = mDisplayAdapter->pauseDisplay(mDisplayPaused);

#if PPM_INSTRUMENTATION || PPM_INSTRUMENTATION_ABS

            mDisplayAdapter->setSnapshotTimeRef(&mStartCapture);

#endif

            }

        if (  (NO_ERROR == ret) && ( NULL != mCameraAdapter ) )
            {
            ret = mCameraAdapter->getPictureBufferSize(pictureBufferLength, bufferCount);

            if ( NO_ERROR != ret )
                {
                CAMHAL_LOGEB("getPictureBufferSize returned error 0x%x", ret);
                }
            }

        if ( NO_ERROR == ret )
            {
            mParameters.getPictureSize(&width, &height);

            ret = allocImageBufs(width, height, pictureBufferLength, mParameters.getPictureFormat(), bufferCount);
            if ( NO_ERROR != ret )
                {
                CAMHAL_LOGEB("allocImageBufs returned error 0x%x", ret);
                }
            }

        if (  (NO_ERROR == ret) && ( NULL != mCameraAdapter ) )
            {
            ret = mCameraAdapter->useBuffers(CameraAdapter::CAMERA_IMAGE_CAPTURE, mImageBufs, mImageOffsets, mImageFd, mImageLength, bufferCount);
            }
        }
    else
        {
        mBracketingRunning = false;
        }

    if ( ( NO_ERROR == ret ) && ( NULL != mCameraAdapter ) )
        {

#if PPM_INSTRUMENTATION || PPM_INSTRUMENTATION_ABS

         //pass capture timestamp along with the camera adapter command
        ret = mCameraAdapter->sendCommand(CameraAdapter::CAMERA_START_IMAGE_CAPTURE,  (int) &mStartCapture);

#else

        ret = mCameraAdapter->sendCommand(CameraAdapter::CAMERA_START_IMAGE_CAPTURE);

#endif

        }

    return ret;
}

/**
   @brief Cancel a picture that was started with takePicture.

   Calling this method when no picture is being taken is a no-op.

   @param none
   @return NO_ERROR If cancel succeeded. Cancel can succeed if image callback is not sent
   @todo Define error codes

 */
status_t CameraHal::cancelPicture( )
{
    LOG_FUNCTION_NAME
    ///@todo Implement this when image capture will be supported
    return NO_ERROR;
}

/**
   @brief Return the camera parameters.

   @param none
   @return Currently configured camera parameters

 */
CameraParameters CameraHal::getParameters() const
{
    CameraParameters params;

     LOG_FUNCTION_NAME

     params = mParameters;
     if( NULL != mCameraAdapter )
        {
        mCameraAdapter->getParameters(params);
        }

     LOG_FUNCTION_NAME_EXIT

     ///Return the current set of parameters

     return params;
}

/**
   @brief Send command to camera driver.

   @param none
   @return NO_ERROR If the command succeeds
   @todo Define the error codes that this function can return

 */
status_t CameraHal::sendCommand(int32_t cmd, int32_t arg1, int32_t arg2)
{
    status_t ret = NO_ERROR;

    LOG_FUNCTION_NAME


    if ( ( NO_ERROR == ret ) && ( NULL == mCameraAdapter ) )
        {
        CAMHAL_LOGEA("No CameraAdapter instance");
        ret = -EINVAL;
        }

    if ( ( NO_ERROR == ret ) && ( !previewEnabled() ))
        {
        CAMHAL_LOGEA("Preview is not running");
        ret = -EINVAL;
        }

    if ( NO_ERROR == ret )
        {
        switch(cmd)
            {
            case CAMERA_CMD_START_SMOOTH_ZOOM:

                ret = mCameraAdapter->sendCommand(CameraAdapter::CAMERA_START_SMOOTH_ZOOM, arg1);

                break;
            case CAMERA_CMD_STOP_SMOOTH_ZOOM:

                ret = mCameraAdapter->sendCommand(CameraAdapter::CAMERA_STOP_SMOOTH_ZOOM);

                break;
            default:
                break;
            };
        }

    LOG_FUNCTION_NAME_EXIT

    return ret;
}

/**
   @brief Release the hardware resources owned by this object.

   Note that this is *not* done in the destructor.

   @param none
   @return none

 */
void CameraHal::release()
{
    LOG_FUNCTION_NAME
    ///@todo Investigate on how release is used by CameraService. Vaguely remember that this is called
    ///just before CameraHal object destruction
    deinitialize();
    LOG_FUNCTION_NAME_EXIT
}


/**
   @brief Dump state of the camera hardware

   @param[in] fd    File descriptor
   @param[in] args  Arguments
   @return NO_ERROR Dump succeeded
   @todo  Error codes for dump fail

 */
status_t  CameraHal::dump(int fd, const Vector<String16>& args) const
{
    LOG_FUNCTION_NAME
    ///Implement this method when the h/w dump function is supported on Ducati side
    return NO_ERROR;
}

/*-------------Camera Hal Interface Method definitions ENDS here--------------------*/




/*-------------Camera Hal Internal Method definitions STARTS here--------------------*/

/**
   @brief Constructor of CameraHal

   Member variables are initialized here.  No allocations should be done here as we
   don't use c++ exceptions in the code.

 */
CameraHal::CameraHal()
{
    LOG_FUNCTION_NAME

    ///Initialize all the member variables to their defaults
    mPreviewBufsAllocatedUsingOverlay = false;
    mPreviewEnabled = false;
    mPreviewBufs = NULL;
    mImageBufs = NULL;
    mBufProvider = NULL;
    mPreviewStartInProgress = false;
    mVideoBufs = NULL;
    mVideoBufProvider = NULL;
    mRecordingEnabled = false;
    mDisplayPaused = false;
    mSetOverlayCalled = false;
    mMsgEnabled = 0;
    mCameraProperties = NULL;
    mAppCallbackNotifier = NULL;
    mMemoryManager = NULL;
    mCameraAdapter = NULL;
    mTakePictureQueue = 0;
    mImageCaptureRunning = false;
    mBracketingEnabled = false;
    mBracketingRunning = false;
    mEventProvider = NULL;
    mBracketRangePositive = 1;
    mBracketRangeNegative = 1;
    mMaxZoomSupported = 0;
    mShutterEnabled = true;
    mMeasurementEnabled = false;
    mPreviewDataBufs = NULL;
    mCameraAdapterHandle = NULL;
    mCameraPropertiesArr = NULL;
    mCurrentTime = 0;
    mFalsePreview = 0;
    mImageOffsets = NULL;
    mImageLength = 0;
    mImageFd = 0;
    mVideoOffsets = NULL;
    mVideoFd = 0;
    mVideoLength = 0;
    mPreviewDataOffsets = NULL;
    mPreviewDataFd = 0;
    mPreviewDataLength = 0;
    mPreviewFd = 0;
    mPreviewWidth = 0;
    mPreviewHeight = 0;
    mPreviewLength = 0;
    mPreviewOffsets = NULL;
    mPreviewRunning = 0;
    mPreviewStateOld = 0;
    mRecordingEnabled = 0;
    mRecordEnabled = 0;

#if PPM_INSTRUMENTATION || PPM_INSTRUMENTATION_ABS

    //Initialize the CameraHAL constructor timestamp, which is used in the
    // PPM() method as time reference if the user does not supply one.
    gettimeofday(&ppm_start, NULL);

#endif

    //Default is set to '0'
    mCameraIndex = 0;

    mReloadAdapter = false;

    LOG_FUNCTION_NAME_EXIT
}

/**
   @brief Destructor of CameraHal

   This function simply calls deinitialize() to free up memory allocate during construct
   phase
 */
CameraHal::~CameraHal()
{
    LOG_FUNCTION_NAME

    ///Call de-initialize here once more - it is the last chance for us to relinquish all the h/w and s/w resources
    deinitialize();

    LOG_FUNCTION_NAME_EXIT
}

/**
   @brief Initialize the Camera HAL

   Creates CameraAdapter, AppCallbackNotifier, DisplayAdapter and MemoryManager

   @param None
   @return NO_ERROR - On success
         NO_MEMORY - On failure to allocate memory for any of the objects
   @remarks Camera Hal internal function

 */
status_t CameraHal::initialize()
{
    LOG_FUNCTION_NAME

    typedef CameraAdapter* (*CameraAdapterFactory)();
    CameraAdapterFactory f = NULL;

    int numCameras = 0;
    int sensor_index = 0;

    ///Initialize the event mask used for registering an event provider for AppCallbackNotifier
    ///Currently, registering all events as to be coming from CameraAdapter
    int32_t eventMask = CameraHalEvent::ALL_EVENTS;

    ///Create the CameraProperties class
    ///We create the default Camera Adapter (The adapter which is selected by default, if no specific camera is chosen using setParameter)
    if(!mCameraProperties.get())
        {
        mCameraProperties = new CameraProperties();
        if(!mCameraProperties.get() || (mCameraProperties->initialize()!=NO_ERROR))
            {
            CAMHAL_LOGEA("Unable to create or initialize CameraProperties");
            goto fail_loop;
            }

        ///Query the number of cameras supported, minimum we expect is one (at least FakeCameraAdapter)
        numCameras = mCameraProperties->camerasSupported();
        if ( 0 == numCameras )
            {
            CAMHAL_LOGEA("No cameras supported in Camera HAL implementation");
            goto fail_loop;
            }
        else
            {
            CAMHAL_LOGDB("Cameras found %d", numCameras);
            }
        }

    ///Get the default Camera
    mCameraPropertiesArr = ( CameraProperties::CameraProperty **) mCameraProperties->getProperties(mCameraIndex);

if(!mCameraPropertiesArr)
    {
    goto fail_loop;
    }

#ifdef DEBUG_LOG

    ///Dump the properties of this Camera
    dumpProperties(mCameraPropertiesArr);

#endif

    ///Check if a valid adapter DLL name is present for the first camera
    if ( NULL == mCameraPropertiesArr[CameraProperties::PROP_INDEX_CAMERA_ADAPTER_DLL_NAME] )
        {
        CAMHAL_LOGEA("No Camera adapter DLL set for default camera");
        goto fail_loop;
        }

    if ( NULL != mCameraPropertiesArr[CameraProperties::PROP_INDEX_CAMERA_SENSOR_INDEX] )
        {
        sensor_index = atoi(mCameraPropertiesArr[CameraProperties::PROP_INDEX_CAMERA_SENSOR_INDEX]->mPropValue);
        }

    CAMHAL_LOGEB("Sensor index %d", sensor_index);


   /// Create the camera adapter
    /// @todo Incorporate dynamic loading based on cfg file. For now using a constant definition for the adapter dll name
    mCameraAdapterHandle = ::dlopen( ( const char * ) mCameraPropertiesArr[CameraProperties::PROP_INDEX_CAMERA_ADAPTER_DLL_NAME]->mPropValue, RTLD_NOW);
    if( NULL  == mCameraAdapterHandle )
        {
        CAMHAL_LOGEB("Unable to open CameraAdapter Library %s for default camera", (const char*)mCameraPropertiesArr[CameraProperties::PROP_INDEX_CAMERA_ADAPTER_DLL_NAME]->mPropValue);
        LOG_FUNCTION_NAME_EXIT
        goto fail_loop;
        }


    f = (CameraAdapterFactory) ::dlsym(mCameraAdapterHandle, "CameraAdapter_Factory");
    if(!f)
        {
        CAMHAL_LOGEB("%s does not export required factory method CameraAdapter_Factory"
            ,mCameraPropertiesArr[CameraProperties::PROP_INDEX_CAMERA_ADAPTER_DLL_NAME]->mPropValue
            );
        goto fail_loop;
        }

    mCameraAdapter = f();
    if ( ( NULL == mCameraAdapter ) || (mCameraAdapter->initialize(sensor_index)!=NO_ERROR))
        {
        CAMHAL_LOGEA("Unable to create or initialize CameraAdapter");
        goto fail_loop;
        }
    mCameraAdapter->sendCommand(CameraAdapter::CAMERA_CANCEL_TIMEOUT);
    mCameraAdapter->registerImageReleaseCallback(releaseImageBuffers, (void *) this);
    mCameraAdapter->registerEndCaptureCallback(endImageCapture, (void *)this);

    if(!mAppCallbackNotifier.get())
        {
        /// Create the callback notifier
        mAppCallbackNotifier = new AppCallbackNotifier();
        if( ( NULL == mAppCallbackNotifier.get() ) || ( mAppCallbackNotifier->initialize() != NO_ERROR))
            {
            CAMHAL_LOGEA("Unable to create or initialize AppCallbackNotifier");
            goto fail_loop;
            }
        }

    if(!mMemoryManager.get())
        {
        /// Create Memory Manager
        mMemoryManager = new MemoryManager();
        if( ( NULL == mMemoryManager.get() ) || ( mMemoryManager->initialize() != NO_ERROR))
            {
            CAMHAL_LOGEA("Unable to create or initialize MemoryManager");
            goto fail_loop;
            }
        }

    ///@remarks Note that the display adapter is not created here because it is dependent on whether overlay
    ///is used by the application or not, for rendering. If the application uses Surface flinger, then it doesnt
    ///pass a surface so, we wont create the display adapter. In this case, we just provide the preview callbacks
    ///to the application for rendering the frames via Surface flinger


    ///Setup the class dependencies...

    ///AppCallbackNotifier has to know where to get the Camera frames and the events like auto focus lock etc from.
    ///CameraAdapter is the one which provides those events
    ///Set it as the frame and event providers for AppCallbackNotifier
    ///@remarks  setEventProvider API takes in a bit mask of events for registering a provider for the different events
    ///         That way, if events can come from DisplayAdapter in future, we will be able to add it as provider
    ///         for any event
    mAppCallbackNotifier->setEventProvider(eventMask, mCameraAdapter);
    mAppCallbackNotifier->setFrameProvider(mCameraAdapter);

    ///Any dynamic errors that happen during the camera use case has to be propagated back to the application
    ///via CAMERA_MSG_ERROR. AppCallbackNotifier is the class that  notifies such errors to the application
    ///Set it as the error handler for CameraAdapter
    mCameraAdapter->setErrorHandler(mAppCallbackNotifier.get());

    ///Initialize default parameters
    initDefaultParameters();


    if ( setParameters(mParameters) != NO_ERROR )
        {
        CAMHAL_LOGEA("Failed to set default parameters?!");
        }

    LOG_FUNCTION_NAME_EXIT

    return NO_ERROR;

    fail_loop:

        ///Free up the resources because we failed somewhere up
        deinitialize();
        LOG_FUNCTION_NAME_EXIT

        return NO_MEMORY;

}

status_t CameraHal::reloadAdapter()
{

    typedef CameraAdapter* (*CameraAdapterFactory)();
    CameraAdapterFactory f = NULL;
    status_t ret = NO_ERROR;
    int sensor_index = 0;

    LOG_FUNCTION_NAME

    if ( mCameraIndex >= mCameraProperties->camerasSupported() )
        {
        CAMHAL_LOGEA("Camera Index exceeds number of supported cameras!");
        ret = -1;
        }

    if ( -1 != ret )
        {
        mCameraPropertiesArr = ( CameraProperties::CameraProperty **) mCameraProperties->getProperties(mCameraIndex);
        }

    if(!mCameraPropertiesArr)
        {
        LOG_FUNCTION_NAME_EXIT
        CAMHAL_LOGEB("getProperties() returned a NULL property set for Camera index %d", mCameraIndex);
        return NO_INIT;
        }
#ifdef DEBUG_LOG

        ///Dump the properties of this Camera
        dumpProperties(mCameraPropertiesArr);

#endif

    ///Check if a valid adapter DLL name is present for the first camera
    if ( NULL == mCameraPropertiesArr[CameraProperties::PROP_INDEX_CAMERA_ADAPTER_DLL_NAME] )
        {
        CAMHAL_LOGEA("No Camera adapter DLL set for default camera");
        ret = -1;
        }

    if ( NULL != mCameraPropertiesArr[CameraProperties::PROP_INDEX_CAMERA_SENSOR_INDEX] )
        {
        sensor_index = atoi(mCameraPropertiesArr[CameraProperties::PROP_INDEX_CAMERA_SENSOR_INDEX]->mPropValue);
        }

    /// Create the camera adapter
    if ( -1 != ret )
        {
        mCameraAdapterHandle = ::dlopen( ( const char * ) mCameraPropertiesArr[CameraProperties::PROP_INDEX_CAMERA_ADAPTER_DLL_NAME]->mPropValue, RTLD_NOW);
        if( NULL  == mCameraAdapterHandle )
            {
            CAMHAL_LOGEB("Unable to open CameraAdapter Library %s for default camera", (const char*)mCameraPropertiesArr[CameraProperties::PROP_INDEX_CAMERA_ADAPTER_DLL_NAME]->mPropValue);
            ret = -1;
            }
        }

    //Delete all existing instances of CameraHAL objects
    //deinitialize();

    initDefaultParameters();

    if ( -1 != ret )
        {
        f = (CameraAdapterFactory) ::dlsym(mCameraAdapterHandle, "CameraAdapter_Factory");
        if(f)
            {
            mCameraAdapter = f();
            if(NULL != mCameraAdapter)
                {
                ret = mCameraAdapter->setParameters(mParameters);
                if(NO_ERROR != mCameraAdapter->initialize(sensor_index))
                    {
                    ret = -1;
                    }

                if(NO_ERROR == ret)
                    {
                    mCameraAdapter->sendCommand(CameraAdapter::CAMERA_CANCEL_TIMEOUT);
                    mCameraAdapter->registerImageReleaseCallback(releaseImageBuffers, (void *) this);
                    mCameraAdapter->registerEndCaptureCallback(endImageCapture, (void *)this);
                    }
                }
            }
        else
            {
            ret = -1;
            }
        }

    if ( (0 == ret) && ( NULL != mAppCallbackNotifier.get() ) &&
           ( NULL !=  mCameraAdapter ) )
        {
        mAppCallbackNotifier->setEventProvider(CameraHalEvent::ALL_EVENTS, mCameraAdapter);
        mAppCallbackNotifier->setFrameProvider(mCameraAdapter);
        mCameraAdapter->setErrorHandler(mAppCallbackNotifier.get());
        }

    if ( (0 == ret) && ( NULL != mDisplayAdapter.get() ) &&
           ( NULL !=  mCameraAdapter ) )
        {
        mDisplayAdapter->setFrameProvider(mCameraAdapter);
        }

    LOG_FUNCTION_NAME_EXIT
    return ret;
}

void CameraHal::dumpProperties(CameraProperties::CameraProperty** cameraProps)
{
    if(!cameraProps)
        {
        CAMHAL_LOGEA("dumpProperties() passed a NULL pointer");
        return;
        }

    for ( int i = 0 ; i < CameraProperties::PROP_INDEX_MAX; i++)
        {
        CAMHAL_LOGDB("%s = %s", cameraProps[i]->mPropName, cameraProps[i]->mPropValue);
        }
}

bool CameraHal::isResolutionValid(unsigned int width, unsigned int height, const char *supportedResolutions)
{
    bool ret = true;
    status_t status = NO_ERROR;
    char tmpBuffer[PARAM_BUFFER + 1];
    char *pos = NULL;

    LOG_FUNCTION_NAME

    if ( NULL == supportedResolutions )
        {
        CAMHAL_LOGEA("Invalid supported resolutions string");
        ret = false;
        goto exit;
        }

    status = snprintf(tmpBuffer, PARAM_BUFFER, "%dx%d", width, height);
    if ( 0 > status )
        {
        CAMHAL_LOGEA("Error encountered while generating validation string");
        ret = false;
        goto exit;
        }

    pos = strstr(supportedResolutions, tmpBuffer);
    if ( NULL == pos )
        {
        ret = false;
        }
    else
        {
        ret = true;
        }

exit:

    LOG_FUNCTION_NAME_EXIT

    return ret;
}

bool CameraHal::isParameterValid(const char *param, const char *supportedParams)
{
    bool ret = true;
    char *pos = NULL;

    LOG_FUNCTION_NAME

    if ( NULL == supportedParams )
        {
        CAMHAL_LOGEA("Invalid supported parameters string");
        ret = false;
        goto exit;
        }

    if ( NULL == param )
        {
        CAMHAL_LOGEA("Invalid parameter string");
        ret = false;
        goto exit;
        }

    pos = strstr(supportedParams, param);
    if ( NULL == pos )
        {
        ret = false;
        }
    else
        {
        ret = true;
        }

exit:

    LOG_FUNCTION_NAME_EXIT

    return ret;
}

bool CameraHal::isParameterValid(int param, const char *supportedParams)
{
    bool ret = true;
    char *pos = NULL;
    status_t status;
    char tmpBuffer[PARAM_BUFFER + 1];

    LOG_FUNCTION_NAME

    if ( NULL == supportedParams )
        {
        CAMHAL_LOGEA("Invalid supported parameters string");
        ret = false;
        goto exit;
        }

    status = snprintf(tmpBuffer, PARAM_BUFFER, "%d", param);
    if ( 0 > status )
        {
        CAMHAL_LOGEA("Error encountered while generating validation string");
        ret = false;
        goto exit;
        }

    pos = strstr(supportedParams, tmpBuffer);
    if ( NULL == pos )
        {
        ret = false;
        }
    else
        {
        ret = true;
        }

exit:

    LOG_FUNCTION_NAME_EXIT

    return ret;
}

status_t CameraHal::parseResolution(const char *resStr, int &width, int &height)
{
    status_t ret = NO_ERROR;
    char *ctx, *pWidth, *pHeight;
    const char *sep = "x";

    LOG_FUNCTION_NAME

    if ( NULL == resStr )
        {
        ret = -EINVAL;
        }

    //This fixes "Invalid input resolution"
    char *resStr_copy = (char *)malloc(strlen(resStr) + 1);
    if ( NULL!=resStr_copy ) {
    if ( NO_ERROR == ret )
        {
        strcpy(resStr_copy, resStr);
        pWidth = strtok_r( (char *) resStr_copy, sep, &ctx);

        if ( NULL != pWidth )
            {
            width = atoi(pWidth);
            }
        else
            {
            CAMHAL_LOGEB("Invalid input resolution %s", resStr);
            ret = -EINVAL;
            }
        }

    if ( NO_ERROR == ret )
        {
        pHeight = strtok_r(NULL, sep, &ctx);

        if ( NULL != pHeight )
            {
            height = atoi(pHeight);
            }
        else
            {
            CAMHAL_LOGEB("Invalid input resolution %s", resStr);
            ret = -EINVAL;
            }
        }

        free(resStr_copy);
        resStr_copy = NULL;
     }
    LOG_FUNCTION_NAME_EXIT

    return ret;
}

void CameraHal::insertSupportedParams()
{
    CameraProperties::CameraProperty **currentProp;
    char tmpBuffer[PARAM_BUFFER + 1];

    LOG_FUNCTION_NAME

    CameraParameters &p = mParameters;

    //Camera Instances supported
    memset(tmpBuffer, '\0', PARAM_BUFFER);
    //Enumerate the supported camera instances
    for ( uint8_t i = 0 ; i < mCameraProperties->camerasSupported() ; i++ )
        {
         currentProp = ( CameraProperties::CameraProperty **) mCameraProperties->getProperties(i);
         if(!currentProp)
            {
            CAMHAL_LOGEB("getProperties returns NULL property set for camera index %d", i);
            LOG_FUNCTION_NAME_EXIT
            continue;
            }
         //Set camera adapter index for reference
         p.set( ( const char * ) currentProp[CameraProperties::PROP_INDEX_CAMERA_NAME]->mPropValue, i);
         strncat( ( char * ) tmpBuffer, ( const char * ) currentProp[CameraProperties::PROP_INDEX_CAMERA_NAME]->mPropValue, INDEX_LENGTH );
         strncat( ( char * ) tmpBuffer, ( const char * ) PARAMS_DELIMITER, INDEX_LENGTH);
         p.set(( const char * ) currentProp[CameraProperties::PROP_INDEX_CAMERA_NAME]->mPropValue, i);
        }
    p.set(TICameraParameters::KEY_SUPPORTED_CAMERAS, tmpBuffer);
    p.set(TICameraParameters::KEY_CAMERA, mCameraIndex);

    ///Set the name of the camera
    p.set(TICameraParameters::KEY_CAMERA_NAME, mCameraPropertiesArr[CameraProperties::PROP_INDEX_CAMERA_NAME]->mPropValue);

    mMaxZoomSupported = atoi(mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_ZOOM_STAGES]->mPropValue);

    p.set(CameraParameters::KEY_SUPPORTED_PICTURE_SIZES, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_PICTURE_SIZES]->mPropValue);
    p.set(CameraParameters::KEY_SUPPORTED_PICTURE_FORMATS, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_PICTURE_FORMATS]->mPropValue);
    p.set(CameraParameters::KEY_SUPPORTED_PREVIEW_SIZES, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_PREVIEW_SIZES]->mPropValue);
    p.set(CameraParameters::KEY_SUPPORTED_PREVIEW_FORMATS, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_PREVIEW_FORMATS]->mPropValue);
    p.set(CameraParameters::KEY_SUPPORTED_PREVIEW_FRAME_RATES, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_PREVIEW_FRAME_RATES]->mPropValue);
    p.set(CameraParameters::KEY_SUPPORTED_JPEG_THUMBNAIL_SIZES, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_THUMBNAIL_SIZES]->mPropValue);
    p.set(CameraParameters::KEY_SUPPORTED_WHITE_BALANCE, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_WHITE_BALANCE]->mPropValue);
    p.set(CameraParameters::KEY_SUPPORTED_EFFECTS, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_EFFECTS]->mPropValue);
    p.set(CameraParameters::KEY_SUPPORTED_SCENE_MODES, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_SCENE_MODES]->mPropValue);
    p.set(CameraParameters::KEY_SUPPORTED_FLASH_MODES, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_FLASH_MODES]->mPropValue);
    p.set(CameraParameters::KEY_SUPPORTED_FOCUS_MODES, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_FOCUS_MODES]->mPropValue);
    p.set(CameraParameters::KEY_SUPPORTED_ANTIBANDING, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_ANTIBANDING]->mPropValue);
    p.set(CameraParameters::KEY_MAX_EXPOSURE_COMPENSATION, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_EV_MAX]->mPropValue);
    p.set(CameraParameters::KEY_MIN_EXPOSURE_COMPENSATION, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_EV_MIN]->mPropValue);
    p.set(CameraParameters::KEY_EXPOSURE_COMPENSATION_STEP, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_EV_STEP]->mPropValue);
    p.set(CameraParameters::KEY_SUPPORTED_SCENE_MODES, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_SCENE_MODES]->mPropValue);
    p.set(TICameraParameters::KEY_SUPPORTED_EXPOSURE, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_EXPOSURE_MODES]->mPropValue);
    p.set(TICameraParameters::KEY_SUPPORTED_ISO_VALUES, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_ISO_VALUES]->mPropValue);
    p.set(CameraParameters::KEY_ZOOM_RATIOS, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_ZOOM_RATIOS]->mPropValue);
    p.set(CameraParameters::KEY_MAX_ZOOM, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_ZOOM_STAGES]->mPropValue);
    p.set(CameraParameters::KEY_ZOOM_SUPPORTED, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_ZOOM_SUPPORTED]->mPropValue);
    p.set(CameraParameters::KEY_SMOOTH_ZOOM_SUPPORTED, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SMOOTH_ZOOM_SUPPORTED]->mPropValue);
    p.set(TICameraParameters::KEY_SUPPORTED_IPP, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SUPPORTED_IPP_MODES]->mPropValue);
    p.set(TICameraParameters::KEY_S3D_SUPPORTED,(const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_S3D_SUPPORTED]->mPropValue);
    p.set(TICameraParameters::KEY_S3D2D_PREVIEW_MODE,(const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_S3D2D_PREVIEW_MODES]->mPropValue);
    p.set(TICameraParameters::KEY_AUTOCONVERGENCE_MODE, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_AUTOCONVERGENCE_MODE]->mPropValue);
    p.set(TICameraParameters::KEY_MANUALCONVERGENCE_VALUES, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_MANUALCONVERGENCE_VALUES]->mPropValue);
    p.set(TICameraParameters::KEY_VSTAB,(const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_VSTAB]->mPropValue);
    p.set(TICameraParameters::KEY_VSTAB_VALUES,(const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_VSTAB_VALUES]->mPropValue);

    LOG_FUNCTION_NAME_EXIT
}

void CameraHal::initDefaultParameters()
{
    //Purpose of this function is to initialize the default current and supported parameters for the currently
    //selected camera.

    CameraParameters &p = mParameters;
    status_t ret = NO_ERROR;
    int width, height;


    LOG_FUNCTION_NAME

    ret = parseResolution((const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_PREVIEW_SIZE]->mPropValue, width, height);

    if ( NO_ERROR == ret )
        {
        p.setPreviewSize(width, height);
        }
    else
        {
        p.setPreviewSize(MIN_WIDTH, MIN_HEIGHT);
        }

    p.setPreviewFrameRate(atoi((const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_PREVIEW_FRAME_RATE]->mPropValue));
    p.setPreviewFormat((const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_PREVIEW_FORMAT]->mPropValue);

    ret = parseResolution((const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_PICTURE_SIZE]->mPropValue, width, height);

    if ( NO_ERROR == ret )
        {
        p.setPictureSize(width, height);
        }
    else
        {
        p.setPictureSize(PICTURE_WIDTH, PICTURE_HEIGHT);
        }

    p.setPictureFormat((const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_PICTURE_FORMAT]->mPropValue);
    p.set(CameraParameters::KEY_JPEG_QUALITY, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_JPEG_QUALITY]->mPropValue);

    ret = parseResolution((const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_JPEG_THUMBNAIL_SIZE]->mPropValue, width, height);

    if ( NO_ERROR == ret )
        {
        p.set(CameraParameters::KEY_JPEG_THUMBNAIL_WIDTH, width);
        p.set(CameraParameters::KEY_JPEG_THUMBNAIL_HEIGHT, height);
        }
    else
        {
        p.set(CameraParameters::KEY_JPEG_THUMBNAIL_WIDTH, MIN_WIDTH);
        p.set(CameraParameters::KEY_JPEG_THUMBNAIL_HEIGHT, MIN_HEIGHT);
        }

    insertSupportedParams();

    //Insert default values
    p.set(CameraParameters::KEY_JPEG_QUALITY, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_JPEG_QUALITY]->mPropValue);
    p.set(CameraParameters::KEY_WHITE_BALANCE, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_WHITEBALANCE]->mPropValue);
    p.set(CameraParameters::KEY_EFFECT,  (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_EFFECT]->mPropValue);
    p.set(CameraParameters::KEY_ANTIBANDING, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_ANTIBANDING]->mPropValue);
    p.set(CameraParameters::KEY_FLASH_MODE, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_FLASH_MODE]->mPropValue);
    p.set(CameraParameters::KEY_FOCUS_MODE, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_FOCUS_MODE]->mPropValue);
    p.set(CameraParameters::KEY_EXPOSURE_COMPENSATION, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_EV_COMPENSATION]->mPropValue);
    p.set(CameraParameters::KEY_SCENE_MODE, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SCENE_MODE]->mPropValue);
    p.set(CameraParameters::KEY_FLASH_MODE, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_FLASH_MODE]->mPropValue);
    p.set(CameraParameters::KEY_ZOOM, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_ZOOM]->mPropValue);
    p.set(TICameraParameters::KEY_CONTRAST, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_CONTRAST]->mPropValue);
    p.set(TICameraParameters::KEY_SATURATION, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SATURATION]->mPropValue);
    p.set(TICameraParameters::KEY_BRIGHTNESS, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_BRIGHTNESS]->mPropValue);
    p.set(TICameraParameters::KEY_SHARPNESS, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_SHARPNESS]->mPropValue);
    p.set(TICameraParameters::KEY_EXPOSURE_MODE, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_EXPOSURE_MODE]->mPropValue);
    p.set(TICameraParameters::KEY_ISO, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_ISO_MODE]->mPropValue);
    p.set(TICameraParameters::KEY_IPP, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_IPP]->mPropValue);
    p.set(TICameraParameters::KEY_S3D2D_PREVIEW, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_S3D2D_PREVIEW]->mPropValue);
    p.set(TICameraParameters::KEY_AUTOCONVERGENCE, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_AUTOCONVERGENCE]->mPropValue);
    p.set(TICameraParameters::KEY_MANUALCONVERGENCE_VALUES, (const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_MANUALCONVERGENCE_VALUES]->mPropValue);
    p.set(TICameraParameters::KEY_VSTAB,(const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_VSTAB]->mPropValue);
    p.set(TICameraParameters::KEY_VSTAB_VALUES,(const char*) mCameraPropertiesArr[CameraProperties::PROP_INDEX_VSTAB_VALUES]->mPropValue);

    LOG_FUNCTION_NAME_EXIT
}

#if PPM_INSTRUMENTATION

/**
   @brief PPM instrumentation

   Dumps the current time offset. The time reference point
   lies within the CameraHAL constructor.

   @param str - log message
   @return none

 */
void CameraHal::PPM(const char* str){
    struct timeval ppm;

    gettimeofday(&ppm, NULL);
    ppm.tv_sec = ppm.tv_sec - ppm_start.tv_sec;
    ppm.tv_sec = ppm.tv_sec * 1000000;
    ppm.tv_sec = ppm.tv_sec + ppm.tv_usec - ppm_start.tv_usec;

    LOGD("PPM: %s :%ld.%ld ms", str, ( ppm.tv_sec /1000 ), ( ppm.tv_sec % 1000 ));
}

#elif PPM_INSTRUMENTATION_ABS

/**
   @brief PPM instrumentation

   Dumps the current time offset. The time reference point
   lies within the CameraHAL constructor. This implemetation
   will also dump the abosolute timestamp, which is useful when
   post calculation is done with data coming from the upper
   layers (Camera application etc.)

   @param str - log message
   @return none

 */
void CameraHal::PPM(const char* str){
    struct timeval ppm;

    unsigned long long elapsed, absolute;
    gettimeofday(&ppm, NULL);
    elapsed = ppm.tv_sec - ppm_start.tv_sec;
    elapsed *= 1000000;
    elapsed += ppm.tv_usec - ppm_start.tv_usec;
    absolute = ppm.tv_sec;
    absolute *= 1000;
    absolute += ppm.tv_usec /1000;

    LOGD("PPM: %s :%llu.%llu ms : %llu ms", str, ( elapsed /1000 ), ( elapsed % 1000 ), absolute);
}

#endif

#if PPM_INSTRUMENTATION || PPM_INSTRUMENTATION_ABS

/**
   @brief PPM instrumentation

   Calculates and dumps the elapsed time using 'ppm_first' as
   reference.

   @param str - log message
   @return none

 */
void CameraHal::PPM(const char* str, struct timeval* ppm_first, ...){
    char temp_str[256];
    struct timeval ppm;
    unsigned long long absolute;
    va_list args;

    va_start(args, ppm_first);
    vsprintf(temp_str, str, args);
    gettimeofday(&ppm, NULL);
    absolute = ppm.tv_sec;
    absolute *= 1000;
    absolute += ppm.tv_usec /1000;
    ppm.tv_sec = ppm.tv_sec - ppm_first->tv_sec;
    ppm.tv_sec = ppm.tv_sec * 1000000;
    ppm.tv_sec = ppm.tv_sec + ppm.tv_usec - ppm_first->tv_usec;

    LOGD("PPM: %s :%ld.%ld ms :  %llu ms", temp_str, ( ppm.tv_sec /1000 ), ( ppm.tv_sec % 1000 ), absolute);

    va_end(args);
}

#endif

/**
   @brief Deallocates memory for all the resources held by Camera HAL.

   Frees the following objects- CameraAdapter, AppCallbackNotifier, DisplayAdapter,
   and Memory Manager

   @param none
   @return none

 */
void CameraHal::deinitialize()
{
    LOG_FUNCTION_NAME

    stopPreview();

    freeImageBufs();

    /// Free the callback notifier
    mAppCallbackNotifier.clear();

    /// Free the memory manager
    mMemoryManager.clear();

    /// Free the display adapter
    mDisplayAdapter.clear();

    // Free the CameraProperties object
    mCameraProperties.clear();

    mSetOverlayCalled = false;

    if ( NULL != mEventProvider )
        {
        mEventProvider->disableEventNotification(CameraHalEvent::ALL_EVENTS);
        delete mEventProvider;
        mEventProvider = NULL;
        }

    if ( NULL != mCameraAdapter )
        {
        mCameraAdapter->sendCommand(CameraAdapter::CAMERA_SET_TIMEOUT, ADAPTER_TIMEOUT);
        mCameraAdapter = NULL;
        }

    ///We dont close the camera adapter DLL here inorder to improve performance
    //::dlclose(mCameraAdapterHandle);

    LOG_FUNCTION_NAME_EXIT

}

/*-------------Camera Hal Internal Method definitions ENDS here--------------------*/

/*--------------------Camera HAL Creation Related---------------------------------------*/

sp<CameraHardwareInterface> CameraHal::createInstance()
{
    LOG_FUNCTION_NAME

    sp<CameraHardwareInterface> hardware(NULL);
    if (singleton != 0)
        {
        hardware = singleton.promote();
        if (hardware != 0)
            {
            CAMHAL_LOGDA("Duplicate instance of Camera");
            ((CameraHal *)hardware.get())->initialize();
            return hardware;
            }
        }

    CameraHal *ptr = new CameraHal();

    if(!ptr)
        {
        CAMHAL_LOGEA("Couldn't create instance of CameraHal class");
        return NULL;
        }

    int r = ptr->initialize();

    if(r!=NO_ERROR)
        {
        delete ptr;
        return NULL;
        }


    singleton = hardware = ptr;

    LOG_FUNCTION_NAME_EXIT
    return hardware;
}


extern "C" sp<CameraHardwareInterface> openCameraHardware()
{
    LOG_FUNCTION_NAME
    CAMHAL_LOGDA("opening ti camera hal\n");
    return CameraHal::createInstance();
}


};


