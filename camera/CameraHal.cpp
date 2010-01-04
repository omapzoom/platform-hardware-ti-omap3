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

#define USE_MEMCOPY_FOR_VIDEO_FRAME 0
#define USE_NEW_OVERLAY 1

#define RES_720P    1280

namespace android {
/*****************************************************************************/

/*
 * This is the overlay_t object, it is returned to the user and represents
 * an overlay. here we use a subclass, where we can store our own state.
 * This handles will be passed across processes and possibly given to other
 * HAL modules (for instance video decode modules).
 */
struct overlay_true_handle_t : public native_handle {
    /* add the data fields we need here, for instance: */
    int ctl_fd;
    int shared_fd;
    int width;
    int height;
    int format;
    int num_buffers;
    int shared_size;
};

/* Defined in liboverlay */
typedef struct {
    int fd;
    size_t length;
    uint32_t offset;
    void *ptr;
} mapping_data_t;

int CameraHal::camera_device = 0;
wp<CameraHardwareInterface> CameraHal::singleton;

CameraHal::CameraHal()
			:mParameters(),
			mRawPictureCallback(0),
			mJpegPictureCallback(0),
			mPictureCallbackCookie(0),
			mOverlay(NULL),
			mPreviewRunning(0),
			mRecordingFrameSize(0),
			mRecordingCallback(0),
			mRecordingCallbackCookie(0),
			mVideoBufferCount(0),
			mVideoHeap(0),
		    mRecordingFrameCount(0),
			mAutoFocusCallback(0),
			mAutoFocusCallbackCookie(0),
			nOverlayBuffersQueued(0),
			nCameraBuffersQueued(0),
			mfirstTime(0),
			pictureNumber(0),
#ifdef FW3A
			fobj(NULL),
#endif
			file_index(0),
			mflash(2),
			mcapture_mode(1),
			mZoomTarget(1),
			mZoomCurrent(1),
			mcaf(0),
			j(0)
{
#if PPM_INSTRUMENTATION
	gettimeofday(&ppm_start, NULL);
#endif

    isStart_FW3A = false;
    isStart_FW3A_AF = false;
    isStart_FW3A_CAF = false;
    isStart_FW3A_AEWB = false;
    isStart_VPP = false;
    isStart_JPEG = false;
    mPictureHeap = NULL;
    mIPPInitAlgoState = false;
    mIPPToEnable = false;
#ifdef IMAGE_PROCESSING_PIPELINE
    pIPP.hIPP = NULL;
#endif
    mIPPInitAlgoState = false;
    mIPPToEnable = false;
    #ifdef IMAGE_PROCESSING_PIPELINE
    pIPP.hIPP = NULL;
    #endif

    int i = 0;
    for(i = 0; i < VIDEO_FRAME_COUNT_MAX; i++)
    {
        mVideoBuffer[i] = 0;
        buffers_queued_to_dss[i] = 0;
    }

    CameraCreate();

    initDefaultParameters();

    CameraConfigure();

    allocatePictureBuffer(PICTURE_WIDTH, PICTURE_HEIGHT);

#ifdef FW3A
    FW3A_Create();
#endif

    ICaptureCreate();

    mPreviewThread = new PreviewThread(this);
    mPreviewThread->run("CameraPreviewThread", PRIORITY_URGENT_DISPLAY);

	if( pipe(procPipe) != 0 ){
		LOGE("Failed creating pipe");
	}
	
	if( pipe(shutterPipe) != 0 ){
		LOGE("Failed creating pipe");
	}
	
	if( pipe(rawPipe) != 0 ){
		LOGE("Failed creating pipe");
	}
	
	if( pipe(snapshotPipe) != 0 ){
		LOGE("Failed creating pipe");
	}

	if( pipe(snapshotReadyPipe) != 0 ){
		LOGE("Failed creating pipe");
	}

	mPROCThread = new PROCThread(this);
    mPROCThread->run("CameraPROCThread", PRIORITY_URGENT_DISPLAY);
	LOGD("STARTING PROC THREAD \n");
    
	mShutterThread = new ShutterThread(this);
    mShutterThread->run("CameraShutterThread", PRIORITY_URGENT_DISPLAY);
	LOGD("STARTING Shutter THREAD \n");

	mRawThread = new RawThread(this);
    mRawThread->run("CameraRawThread", PRIORITY_URGENT_DISPLAY);
	LOGD("STARTING Raw THREAD \n");

	mSnapshotThread = new SnapshotThread(this);
    mSnapshotThread->run("CameraSnapshotThread", PRIORITY_URGENT_DISPLAY);
	LOGD("STARTING Snapshot THREAD \n");

#ifdef FW3A
    if (fobj!=NULL)
    {
        FW3A_DefaultSettings();
    }
#endif

}

void CameraHal::initDefaultParameters()
{
    CameraParameters p;
 
    LOG_FUNCTION_NAME
	
    p.setPreviewSize(MIN_WIDTH, MIN_HEIGHT);
    p.setPreviewFrameRate(30);
    p.setPreviewFormat("yuv422i");

    p.setPictureSize(PICTURE_WIDTH, PICTURE_HEIGHT);
    p.setPictureFormat("jpeg");
    p.set("jpeg-quality", 100);    
    if (setParameters(p) != NO_ERROR) {
        LOGE("Failed to set default parameters?!");
    }

    LOG_FUNCTION_NAME_EXIT

}


CameraHal::~CameraHal()
{
    int err = 0;
	int procMessage [1];
	sp<PROCThread> procThread;
	sp<RawThread> rawThread;
	sp<ShutterThread> shutterThread;
	sp<SnapshotThread> snapshotThread;
	  
    LOG_FUNCTION_NAME
	

    if(mPreviewThread != NULL) {
        Message msg;
        msg.command = PREVIEW_KILL;
        previewThreadCommandQ.put(&msg);
        previewThreadAckQ.get(&msg);
    }
  
    sp<PreviewThread> previewThread;
    
    { // scope for the lock
        Mutex::Autolock lock(mLock);
        previewThread = mPreviewThread;
    }

    // don't hold the lock while waiting for the thread to quit
    if (previewThread != 0) {
        previewThread->requestExitAndWait();
    }

    { // scope for the lock
        Mutex::Autolock lock(mLock);
        mPreviewThread.clear();
    }

	procMessage[0] = PROC_THREAD_EXIT;
	write(procPipe[1], procMessage, sizeof(unsigned int));

	{ // scope for the lock
        Mutex::Autolock lock(mLock);
        procThread = mPROCThread;
    }

    // don't hold the lock while waiting for the thread to quit
    if (procThread != 0) {
        procThread->requestExitAndWait();
    }

    { // scope for the lock
        Mutex::Autolock lock(mLock);
        mPROCThread.clear();
        close(procPipe[0]);
        close(procPipe[1]);
    }

	procMessage[0] = SHUTTER_THREAD_EXIT;
	write(shutterPipe[1], procMessage, sizeof(unsigned int));

	{ // scope for the lock
        Mutex::Autolock lock(mLock);
        shutterThread = mShutterThread;
    }

    // don't hold the lock while waiting for the thread to quit
    if (shutterThread != 0) {
        shutterThread->requestExitAndWait();
    }

    { // scope for the lock
        Mutex::Autolock lock(mLock);
        mShutterThread.clear();
        close(shutterPipe[0]);
        close(shutterPipe[1]);
    }
    
	procMessage[0] = RAW_THREAD_EXIT;
	write(rawPipe[1], procMessage, sizeof(unsigned int));

	{ // scope for the lock
        Mutex::Autolock lock(mLock);
        rawThread = mRawThread;
    }

    // don't hold the lock while waiting for the thread to quit
    if (rawThread != 0) {
        rawThread->requestExitAndWait();
    }

    { // scope for the lock
        Mutex::Autolock lock(mLock);
        mRawThread.clear();
        close(rawPipe[0]);
        close(rawPipe[1]);
    }

    procMessage[0] = SNAPSHOT_THREAD_EXIT;
    write(snapshotPipe[1], procMessage, sizeof(unsigned int));

    {
        Mutex::Autolock lock(mLock);
        snapshotThread = mSnapshotThread;
    }

    if (snapshotThread != 0 ) {
        snapshotThread->requestExitAndWait();
    }

    {
        Mutex::Autolock lock(mLock);
        mSnapshotThread.clear();
        close(snapshotPipe[0]);
        close(snapshotPipe[1]);
        close(snapshotReadyPipe[0]);
        close(snapshotReadyPipe[1]);
    }

    ICaptureDestroy();

#ifdef FW3A
    FW3A_Destroy();
#endif

    CameraDestroy();

#ifdef IMAGE_PROCESSING_PIPELINE
    if(pIPP.hIPP != NULL){
        err = DeInitIPP();
        if( err ) {
            LOGE("ERROR DeInitIPP() failed");
        }
        pIPP.hIPP = NULL;
	}
#endif

    if ( mOverlay.get() != NULL )
    {
        LOGD("Destroying current overlay");
        mOverlay->destroy();
    }
    
    LOGD("<<< Release");

    singleton.clear();
}

void CameraHal::previewThread()
{
    Message msg;
    bool  shouldLive = true;
    bool has_message;
    int err; 

    LOG_FUNCTION_NAME
    
    while(shouldLive) {
    
        has_message = false;

        if( mPreviewRunning )
        {
            //process 1 preview frame
            nextPreview();

#ifdef FW3A
            if (isStart_FW3A_AF) {
                err = fobj->cam_iface_2a->ReadSatus(fobj->cam_iface_2a->pPrivateHandle, &fobj->status_2a);
                if ((err == 0) && (AF_STATUS_RUNNING != fobj->status_2a.af.status)) {
			#if PPM_INSTRUMENTATION	
				PPM("AF Completed in ",&focus_before);
			#endif
				
                    fobj->cam_iface_2a->ReadMakerNote(fobj->cam_iface_2a->pPrivateHandle, ancillary_buffer, (uint32 *) &ancillary_len);
                    if (FW3A_Stop_AF() < 0){
						LOGE("ERROR FW3A_Stop_AF()");						
					}
                    mAutoFocusCallback( true, mAutoFocusCallbackCookie );
                }
            }
#endif

            if( !previewThreadCommandQ.isEmpty() ) {
                previewThreadCommandQ.get(&msg);
                has_message = true;
            }
        }
        else
        {
            //block for message
            previewThreadCommandQ.get(&msg);
            has_message = true;
        }

        if( !has_message )
            continue;

        switch(msg.command)
        {
            case PREVIEW_START:
            {
                LOGD("Receive Command: PREVIEW_START");              
                err = 0;

                if( ! mPreviewRunning ) {

                    PPM("CONFIGURING CAMERA TO RESTART PREVIEW");
					if (CameraConfigure() < 0){
						LOGE("ERROR CameraConfigure()");
						err= -1;
					}
#ifdef FW3A
                    if (FW3A_Start() < 0){
						LOGE("ERROR FW3A_Start()");
						err= -1;
					}
                    if (FW3A_SetSettings() < 0){
						LOGE("ERROR FW3A_SetSettings()");
						err= -1;
					}
                    
#endif
                    if (CameraStart() < 0){
						LOGE("ERROR CameraStart()");
						err = -1;
					}   
     
					if(!mfirstTime){
						PPM("Standby to first shot");					
						mfirstTime++;
					}
					else{			
					#if PPM_INSTRUMENTATION		
						PPM("Shot to Shot", &ppm_receiveCmdToTakePicture);					
					#endif
					}
                }
                else
                {
                    err = -1;
                }

                LOGD("PREVIEW_START %s", err ? "NACK" : "ACK");
                msg.command = err ? PREVIEW_NACK : PREVIEW_ACK;

                if( !err ){
                    LOGD("Preview Started!");
                    mPreviewRunning = true;
                }

                previewThreadAckQ.put(&msg);
            }
            break;

            case PREVIEW_STOP:
            {
				LOGD("Receive Command: PREVIEW_STOP");
				err = 0;
                if( mPreviewRunning ) {
#ifdef FW3A					
                    if( FW3A_Stop_AF() < 0){
						LOGE("ERROR FW3A_Stop_AF()");
						err= -1;
					}
					if( FW3A_Stop_CAF() < 0){
						LOGE("ERROR FW3A_Stop_CAF()");
						err= -1;
					}
					if( FW3A_Stop() < 0){
						LOGE("ERROR FW3A_Stop()");
						err= -1;
					}
					if( FW3A_GetSettings() < 0){
						LOGE("ERROR FW3A_GetSettings()");
						err= -1;
					}               
#endif
					if( CameraStop() < 0){
						LOGE("ERROR CameraStop()");
						err= -1;
					}
                   
                    if (err) {
                        LOGE("ERROR Cannot deinit preview.");
                    }
                    LOGD("PREVIEW_STOP %s", err ? "NACK" : "ACK");
                    msg.command = err ? PREVIEW_NACK : PREVIEW_ACK;
                }
                else
                {
                    msg.command = PREVIEW_NACK;
                }

                mPreviewRunning = false;

                previewThreadAckQ.put(&msg);
            }
            break;
	
			case PREVIEW_AF_START:
            {
                LOGD("Receive Command: PREVIEW_AF_START");
				err = 0;

                if( !mPreviewRunning ){
                    LOGD("WARNING PREVIEW NOT RUNNING!");
                    msg.command = PREVIEW_NACK;
                }
                else
                {
                    mAutoFocusCallback       = (autofocus_callback) msg.arg1;
                    mAutoFocusCallbackCookie = msg.arg2;
#ifdef FW3A   

					if (isStart_FW3A_CAF!= 0){
						if( FW3A_Stop_CAF() < 0){
							LOGE("ERROR FW3A_Stop_CAF();");
							err = -1;
						}
                    }
				#if PPM_INSTRUMENTATION
					gettimeofday(&focus_before, NULL);
				#endif
					if (isStart_FW3A_AF == 0){
						if( FW3A_Start_AF() < 0){
							LOGE("ERROR FW3A_Start_AF()");
							err = -1;
						}
        
					}
#endif  
					mAutoFocusCallback( true, mAutoFocusCallbackCookie );
                    msg.command = err ? PREVIEW_NACK : PREVIEW_ACK;   
					                
                }
                LOGD("Receive Command: PREVIEW_AF_START %s", msg.command == PREVIEW_NACK ? "NACK" : "ACK"); 
                previewThreadAckQ.put(&msg);
            }
            break;
		
			case PREVIEW_CAF_START:
			{	
                LOGD("Receive Command: PREVIEW_CAF_START");
				err=0;

                if( !mPreviewRunning )
                    msg.command = PREVIEW_NACK;
                else
                {
#ifdef FW3A    
					if( FW3A_Start_CAF() < 0){
						LOGE("ERROR FW3A_Start_CAF()");
						err = -1;
					} 
#endif
                    msg.command = err ? PREVIEW_NACK : PREVIEW_ACK;
                }
                LOGD("Receive Command: PREVIEW_CAF_START %s", msg.command == PREVIEW_NACK ? "NACK" : "ACK"); 
                previewThreadAckQ.put(&msg);
			}
			break;
                
			case PREVIEW_CAF_STOP:
			{
                LOGD("Receive Command: PREVIEW_CAF_STOP");
				err = 0;

                if( !mPreviewRunning )
                    msg.command = PREVIEW_NACK;
                else
                {
#ifdef FW3A    
					if( FW3A_Stop_CAF() < 0){
						LOGE("ERROR FW3A_Stop_CAF()");
						err = -1;
					} 
#endif
                    msg.command = err ? PREVIEW_NACK : PREVIEW_ACK;
                }
                LOGD("Receive Command: PREVIEW_CAF_STOP %s", msg.command == PREVIEW_NACK ? "NACK" : "ACK"); 
                previewThreadAckQ.put(&msg);
			}
			break;
	
            case PREVIEW_CAPTURE:
            {
                int flg_AF;
                int flg_CAF;
                err = 0;

#ifdef DEBUG_LOG

                LOGD("ENTER OPTION PREVIEW_CAPTURE");

				PPM("RECEIVED COMMAND TO TAKE A PICTURE");

#endif

			#if PPM_INSTRUMENTATION
				gettimeofday(&ppm_receiveCmdToTakePicture, NULL);
			#endif
                
                mShutterCallback    = (shutter_callback)msg.arg1;
                mRawPictureCallback = (raw_callback)msg.arg2;
                mJpegPictureCallback= (jpeg_callback)msg.arg3;
                mPictureCallbackCookie = msg.arg4;

                //In burst mode the preview is not reconfigured between each picture 
                //so it can not be based on it to decide wheter the state is incorrect or not
                msg.command = PREVIEW_ACK;
                previewThreadAckQ.put(&msg);
        
                if( mPreviewRunning ) {

#ifdef OPP_OPTIMIZATION

                    if ( RMProxy_RequestBoost(MAX_BOOST) != OMX_ErrorNone ) {
                        LOGE("OPP Boost failed");
                    } else {
                        LOGE("OPP Boost success");
                    }

#endif

				    if( CameraStop() < 0){
					    LOGE("ERROR CameraStop()");
					    err = -1;
				    }

#ifdef FW3A     
				    if( (flg_AF = FW3A_Stop_AF()) < 0){
					    LOGE("ERROR FW3A_Stop_AF()");
					    err = -1;
				    }            
				    if( (flg_CAF = FW3A_Stop_CAF()) < 0){
					    LOGE("ERROR FW3A_Stop_CAF()");
					    err = -1;
				    }
				    if( FW3A_Stop() < 0){
					    LOGE("ERROR FW3A_Stop()");
					    err = -1;
				    }          
#endif         

                    mPreviewRunning = false;

                }
#ifdef FW3A
				if( FW3A_GetSettings() < 0){
					LOGE("ERROR FW3A_GetSettings()");
					err = -1;
				}
#endif

#ifdef DEBUG_LOG

                PPM("STOPPED PREVIEW");

#endif

#ifdef ICAP
				if( ICapturePerform() < 0){
					LOGE("ERROR ICapturePerform()");
					err = -1;
				}     
#else
				if( CapturePicture() < 0){
					LOGE("ERROR CapturePicture()");
					err = -1;
				}   
#endif
                if( err )
                    LOGE("Capture failed.");

                //restart the preview

#ifdef DEBUG_LOG

                PPM("CONFIGURING CAMERA TO RESTART PREVIEW");

#endif

				if (CameraConfigure() < 0)
					LOGE("ERROR CameraConfigure()");

#ifdef FW3A

                if (FW3A_Start() < 0)
					LOGE("ERROR FW3A_Start()");

                if (FW3A_SetSettings() < 0)
					LOGE("ERROR FW3A_SetSettings()");
                    
#endif
                if (CameraStart() < 0)
					LOGE("ERROR CameraStart()");

#if PPM_INSTRUMENTATION		

				PPM("Shot to Shot", &ppm_receiveCmdToTakePicture);					

#endif

#ifdef ICAP_EXPERIMENTAL

                allocatePictureBuffer(iobj->cfg.sizeof_img_buf);

#else
            
                allocatePictureBuffer(PICTURE_WIDTH, PICTURE_HEIGHT);
 
#endif

                mPreviewRunning = true;

                LOGD("EXIT OPTION PREVIEW_CAPTURE");
            }
            break;

            case PREVIEW_CAPTURE_CANCEL:
            {
                LOGD("Receive Command: PREVIEW_CAPTURE_CANCEL");
                msg.command = PREVIEW_NACK;
                previewThreadAckQ.put(&msg);
            }
            break;

            case PREVIEW_KILL:
            {
                LOGD("Receive Command: PREVIEW_KILL");
				err = 0;				
	
                if (mPreviewRunning) {
#ifdef FW3A 
					if( FW3A_Stop_AF() < 0){
						LOGE("ERROR FW3A_Stop_AF()");
						err = -1;
					}
					if( FW3A_Stop_CAF() < 0){
						LOGE("ERROR FW3A_Stop_CAF()");
						err = -1;
					}
					if( FW3A_Stop() < 0){
						LOGE("ERROR FW3A_Stop()");
						err = -1;
					}
#endif 
					if( CameraStop() < 0){
						LOGE("ERROR FW3A_Stop()");
						err = -1;
					}
                    mPreviewRunning = false;
                }
	
				msg.command = err ? PREVIEW_NACK : PREVIEW_ACK;
                LOGD("Receive Command: PREVIEW_CAF_STOP %s", msg.command == PREVIEW_NACK ? "NACK" : "ACK"); 

                previewThreadAckQ.put(&msg);
                shouldLive = false;
            }
            break;
        }
    }

   LOG_FUNCTION_NAME_EXIT
}

int CameraHal::CameraCreate()
{
    int err = 0;

    LOG_FUNCTION_NAME

    camera_device = open(VIDEO_DEVICE, O_RDWR);
    if (camera_device < 0) {
        LOGE ("Could not open the camera device: %s",  strerror(errno) );
        goto exit;
    }

    LOG_FUNCTION_NAME_EXIT
    return 0;

exit:
    return err;
}


int CameraHal::CameraDestroy()
{
    int err;	

	LOG_FUNCTION_NAME

    close(camera_device);
    camera_device = -1;

    if (mOverlay != NULL) {
        mOverlay->destroy();
        mOverlay = NULL;
        nOverlayBuffersQueued = 0;
    }

    LOG_FUNCTION_NAME_EXIT
    return 0;
}

int CameraHal::CameraConfigure()
{
    int w, h, framerate;
    int image_width, image_height;
    int err;
    struct v4l2_format format;
    enum v4l2_buf_type type;
    struct v4l2_control vc;
    struct v4l2_streamparm parm;

    LOG_FUNCTION_NAME

    mParameters.getPreviewSize(&w, &h);
   
    /* Set preview format */
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = w;
    format.fmt.pix.height = h;
    format.fmt.pix.pixelformat = PIXEL_FORMAT;

    err = ioctl(camera_device, VIDIOC_S_FMT, &format);
    if ( err < 0 ){
        LOGE ("Failed to set VIDIOC_S_FMT.");
        goto s_fmt_fail;
    }

    LOGI("CameraConfigure PreviewFormat: w=%d h=%d", format.fmt.pix.width, format.fmt.pix.height);	

    framerate = mParameters.getPreviewFrameRate();
    
    LOGD("CameraConfigure: framerate to set = %d",framerate);
  
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    err = ioctl(camera_device, VIDIOC_G_PARM, &parm);
    if(err != 0) {
		LOGD("VIDIOC_G_PARM ");
		return -1;
    }
    
    LOGD("CameraConfigure: Old frame rate is %d/%d  fps",
		parm.parm.capture.timeperframe.denominator,
		parm.parm.capture.timeperframe.numerator);
  
	parm.parm.capture.timeperframe.numerator = 1;
	parm.parm.capture.timeperframe.denominator = framerate;
	err = ioctl(camera_device, VIDIOC_S_PARM, &parm);
	if(err != 0) {
		LOGE("VIDIOC_S_PARM ");
		return -1;
	}
    
    LOGI("CameraConfigure: New frame rate is %d/%d fps",parm.parm.capture.timeperframe.denominator,parm.parm.capture.timeperframe.numerator);

    LOG_FUNCTION_NAME_EXIT
    return 0;

s_fmt_fail:
    return -1;
}

int CameraHal::CameraStart()
{
    int w, h;
    int err;
    int nSizeBytes;
    int buffer_count;
    struct v4l2_format format;
    enum v4l2_buf_type type;
    struct v4l2_requestbuffers creqbuf;

    LOG_FUNCTION_NAME

    nCameraBuffersQueued = 0;  

    mParameters.getPreviewSize(&w, &h);
    LOGD("**CaptureQBuffers: preview size=%dx%d", w, h);
  
    mPreviewFrameSize = w * h * 2;
    if (mPreviewFrameSize & 0xfff)
    {
        mPreviewFrameSize = (mPreviewFrameSize & 0xfffff000) + 0x1000;
    }
    LOGD("mPreviewFrameSize = 0x%x = %d", mPreviewFrameSize, mPreviewFrameSize);

    buffer_count = mOverlay->getBufferCount();
    LOGD("number of buffers = %d\n", buffer_count);

    creqbuf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    creqbuf.memory = V4L2_MEMORY_USERPTR;
    creqbuf.count  =  buffer_count ; 
    if (ioctl(camera_device, VIDIOC_REQBUFS, &creqbuf) < 0) {
        LOGE ("VIDIOC_REQBUFS Failed. %s", strerror(errno));
        goto fail_reqbufs;
    }

    for (int i = 0; i < (int)creqbuf.count; i++) {

        v4l2_cam_buffer[i].type = creqbuf.type;
        v4l2_cam_buffer[i].memory = creqbuf.memory;
        v4l2_cam_buffer[i].index = i;

        if (ioctl(camera_device, VIDIOC_QUERYBUF, &v4l2_cam_buffer[i]) < 0) {
            LOGE("VIDIOC_QUERYBUF Failed");
            goto fail_loop;
        }
      
#if USE_NEW_OVERLAY
        mapping_data_t* data = (mapping_data_t*) mOverlay->getBufferAddress((void*)i);
        if ( data == NULL ) {
            LOGE(" getBufferAddress returned NULL");
            goto fail_loop;
        }

        v4l2_cam_buffer[i].m.userptr = (unsigned long) data->ptr;
#else
        v4l2_cam_buffer[i].m.userptr = (unsigned long) mOverlay->getBufferAddress((void*)i);
        if ( v4l2_cam_buffer[i].m.userptr == NULL ) {
            LOGE(" getBufferAddress returned NULL");
            goto fail_loop;
        }
#endif
        strcpy((char *)v4l2_cam_buffer[i].m.userptr, "hello");
        if (strcmp((char *)v4l2_cam_buffer[i].m.userptr, "hello")) {
            LOGI("problem with buffer\n");
            goto fail_loop;
        }

        LOGD("User Buffer [%d].start = %p  length = %d\n", i,
             (void*)v4l2_cam_buffer[i].m.userptr, v4l2_cam_buffer[i].length);

        if (buffers_queued_to_dss[i] == 0)
        {
            nCameraBuffersQueued++;
            if (ioctl(camera_device, VIDIOC_QBUF, &v4l2_cam_buffer[i]) < 0) {
                LOGE("CameraStart VIDIOC_QBUF Failed: %s", strerror(errno) );
                goto fail_loop;
            }
         }
         else LOGI("CameraStart::Could not queue buffer %d to Camera because it is being held by Overlay", i);
         
    }

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    err = ioctl(camera_device, VIDIOC_STREAMON, &type);
    if ( err < 0) {
        LOGE("VIDIOC_STREAMON Failed");
        goto fail_loop;
    }

    if ( mZoomTarget != mZoomCurrent ) {
        
        if( ZoomPerform(mZoomTarget) < 0 )
            LOGE("Error while applying zoom");   
        
        mZoomCurrent = mZoomTarget;
    }

    LOG_FUNCTION_NAME_EXIT
    return 0;

fail_bufalloc:
fail_loop:
fail_reqbufs:

    return -1;
}

int CameraHal::CameraStop()
{

#ifdef DEBUG_LOG

    LOG_FUNCTION_NAME

#endif

    int ret;
    struct v4l2_requestbuffers creqbuf;
    struct v4l2_buffer cfilledbuffer;
    cfilledbuffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    cfilledbuffer.memory = V4L2_MEMORY_USERPTR;

    while(nCameraBuffersQueued){
        nCameraBuffersQueued--;
    }

#ifdef DEBUG_LOG

	LOGD("Done dequeuing from Camera!");

#endif

    creqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(camera_device, VIDIOC_STREAMOFF, &creqbuf.type) == -1) {
        LOGE("VIDIOC_STREAMOFF Failed");
        goto fail_streamoff;
    }

	//Force the zoom to be updated next time preview is started.
	mZoomCurrent = 1;

#ifdef DEBUG_LOG

    LOG_FUNCTION_NAME_EXIT

#endif

    return 0;

fail_streamoff:

    return -1;
}

void CameraHal::nextPreview()
{
    struct v4l2_buffer cfilledbuffer;
    cfilledbuffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    cfilledbuffer.memory = V4L2_MEMORY_USERPTR;
    int w, h, ret, queue_to_dss_failed;

    overlay_buffer_t overlaybuffer;// contains the index of the buffer dque
    int overlaybufferindex = -1; //contains the last buffer dque or -1 if dque failed
    int index;
    recording_callback cb = NULL;
	
	//LOG_FUNCTION_NAME

    mParameters.getPreviewSize(&w, &h);	

    /* De-queue the next avaliable buffer */	
    if (ioctl(camera_device, VIDIOC_DQBUF, &cfilledbuffer) < 0) {
        LOGE("VIDIOC_DQBUF Failed!!!");
		goto EXIT;
    }else{
	    nCameraBuffersQueued--;
	}

    queue_to_dss_failed = mOverlay->queueBuffer((void*)cfilledbuffer.index);

    if (queue_to_dss_failed)
	{
		LOGE("nextPreview(): mOverlay->queueBuffer() failed:[%d]",cfilledbuffer.index);
	}
	else
	{
	    nOverlayBuffersQueued++;
	    buffers_queued_to_dss[cfilledbuffer.index] = 1; //queued
	}

    if (nOverlayBuffersQueued >= NUM_BUFFERS_TO_BE_QUEUED_FOR_OPTIMAL_PERFORMANCE)
    {
		
        if(mOverlay->dequeueBuffer(&overlaybuffer)){
			LOGE("nextPreview(): mOverlay->dequeueBuffer() failed");
		}
		else{
        	overlaybufferindex = (int)overlaybuffer;
            nOverlayBuffersQueued--;
            buffers_queued_to_dss[(int)overlaybuffer] = 0;
            lastOverlayBufferDQ = (int)overlaybuffer;	
        }
    }
    else
    {
        //cfilledbuffer.index = whatever was in there before..
        //That is, queue the same buffer that was dequeued
    }

    mRecordingLock.lock();
    cb = mRecordingCallback;

    if(cb){
        nsecs_t timeStamp = systemTime(SYSTEM_TIME_MONOTONIC);
#if USE_MEMCOPY_FOR_VIDEO_FRAME
        for(int i = 0 ; i < VIDEO_FRAME_COUNT_MAX; i++ ){
            if(0 == mVideoBufferUsing[i]){
                memcpy(mVideoBuffer[i]->pointer(),(void *)cfilledbuffer.m.userptr, mRecordingFrameSize);
                mVideoBufferUsing[i] = 1;
                cb(timeStamp, mVideoBuffer[i], mRecordingCallbackCookie);
                break;
            }else {
                LOGE("No Buffer Can be used!");
            }
        }

        if (queue_to_dss_failed) {		
            if (ioctl(camera_device, VIDIOC_QBUF, &cfilledbuffer) < 0) {
                LOGE("VIDIOC_QBUF Failed, line=%d",__LINE__);
            }else{
	            nCameraBuffersQueued++;
			}
        }
        else if (overlaybufferindex != -1) {				
            if (ioctl(camera_device, VIDIOC_QBUF, &v4l2_cam_buffer[(int)overlaybuffer]) < 0) {
                LOGE("VIDIOC_QBUF Failed. line=%d",__LINE__);
            }else{
	            nCameraBuffersQueued++;
			}
        }

#else
        if(overlaybufferindex != -1){
            //LOGE("<Dqueue>... [index]=%d",cfilledbuffer.index);
            //LOGE("<Recording>... [index]=%d",overlaybuffer);
            cb(timeStamp, mVideoBuffer[(int)overlaybuffer], mRecordingCallbackCookie);
        }
#endif
    } 
    else {
        if (queue_to_dss_failed) {			
            if (ioctl(camera_device, VIDIOC_QBUF, &cfilledbuffer) < 0) {
                LOGE("VIDIOC_QBUF Failed, line=%d",__LINE__);
            }else{
	            nCameraBuffersQueued++;
			}
        }
        else if (overlaybufferindex != -1) {		
            if (ioctl(camera_device, VIDIOC_QBUF, &v4l2_cam_buffer[(int)overlaybuffer]) < 0) {
                LOGE("VIDIOC_QBUF Failed. line=%d",__LINE__);
            }else{
	            nCameraBuffersQueued++;
			}
        }
    }

    mRecordingLock.unlock();

EXIT:

    return ;
}
	
#ifdef ICAP
int  CameraHal::ICapturePerform()
{

    int err;
    int status = 0;
    int jpegSize;
    void *outBuffer;  
    unsigned long base, offset, jpeg_offset;
    int image_width, image_height;
    struct manual_parameters  manual_config;
    unsigned int    procMessage[PROC_THREAD_NUM_ARGS],
                    shutterMessage[SHUTTER_THREAD_NUM_ARGS],
                    rawMessage[RAW_THREAD_NUM_ARGS];
	int pixelFormat;

#ifdef DEBUG_LOG

    LOG_FUNCTION_NAME

    PPM("START OF ICapturePerform");

#endif

    mParameters.getPictureSize(&image_width, &image_height);

#ifdef DEBUG_LOG

    LOGD("ICapturePerform image_width=%d image_height=%d",image_width,image_height);

#endif

    memset(&manual_config, 0 ,sizeof(manual_config));

#ifdef DEBUG_LOG

	LOGD("ICapturePerform beforeReadStatus");

#endif

    err = fobj->cam_iface_2a->ReadSatus(fobj->cam_iface_2a->pPrivateHandle, &fobj->status_2a);

#ifdef DEBUG_LOG

    LOGD("shutter_cap = %d ; again_cap = %d ; awb_index = %d; %d %d %d %d\n",
        (int)fobj->status_2a.ae.shutter_cap, (int)fobj->status_2a.ae.again_cap,
        (int)fobj->status_2a.awb.awb_index, (int)fobj->status_2a.awb.gain_Gr,
        (int)fobj->status_2a.awb.gain_R, (int)fobj->status_2a.awb.gain_B,
        (int)fobj->status_2a.awb.gain_Gb);

#endif

    manual_config.shutter_usec      = fobj->status_2a.ae.shutter_cap;
    manual_config.analog_gain       = fobj->status_2a.ae.again_cap;
    manual_config.color_temparature = fobj->status_2a.awb.awb_index;
    manual_config.gain_Gr           = fobj->status_2a.awb.gain_Gr;
    manual_config.gain_R            = fobj->status_2a.awb.gain_R;
    manual_config.gain_B            = fobj->status_2a.awb.gain_B;
    manual_config.gain_Gb           = fobj->status_2a.awb.gain_Gb;
    manual_config.digital_gain      = fobj->status_2a.awb.dgain;   
    manual_config.scene             = fobj->settings_2a.general.scene;
    manual_config.effect            = fobj->settings_2a.general.effects;
    manual_config.awb_mode          = fobj->settings_2a.awb.mode;
    manual_config.sharpness         = fobj->settings_2a.general.sharpness;
    manual_config.brightness        = fobj->settings_2a.general.brightness;
    manual_config.contrast          = fobj->settings_2a.general.contrast;
    manual_config.saturation        = fobj->settings_2a.general.saturation;


#ifdef DEBUG_LOG

    PPM("SETUP SOME 3A STUFF");

#endif

    iobj->cfg.image_width   = image_width;
    iobj->cfg.image_height  = image_height;
    iobj->cfg.lsc_type      = LSC_UPSAMPLED_BY_SOFTWARE;
    iobj->cfg.cam_dev       = camera_device;
    iobj->cfg.mknote        = ancillary_buffer;
    iobj->cfg.manual        = &manual_config;
    iobj->cfg.priv          = this;
    iobj->cfg.cb_write_h3a  = NULL;//onSaveH3A;
    iobj->cfg.cb_write_lsc  = NULL;//onSaveLSC;
    iobj->cfg.cb_write_raw  = NULL;//onSaveRAW;
    iobj->cfg.cb_picture_done = onSnapshot;
    manual_config.pre_flash = 0;

    if(mcapture_mode == 1)
        iobj->cfg.capture_mode  =  CAPTURE_MODE_HI_PERFORMANCE;
    else
        iobj->cfg.capture_mode  =  CAPTURE_MODE_HI_QUALITY;

#if DEBUG_LOG

	PPM("Before ICapture Config");

#endif

    status = (capture_status_t) iobj->lib.Config(iobj->lib_private, &iobj->cfg);

    if( ICAPTURE_FAIL == status){
        LOGE ("ICapture Config function failed");
        goto fail_config;
    }

#if DEBUG_LOG

    PPM("ICapture config OK");

	LOGD("iobj->cfg.image_width = %d = 0x%x iobj->cfg.image_height=%d = 0x%x , iobj->cfg.sizeof_img_buf = %d", (int)iobj->cfg.image_width, (int)iobj->cfg.image_width, (int)iobj->cfg.image_height, (int)iobj->cfg.image_height, (int)iobj->cfg.sizeof_img_buf);

#endif

    yuv_buffer = (uint8_t *) mYuvBuffer;
    offset = mPictureOffset;
    yuv_len = mPictureLength;

    iobj->proc.img_buf[0].start = yuv_buffer; 
    iobj->proc.img_buf[0].length = iobj->cfg.sizeof_img_buf;
    iobj->proc.img_bufs_count = 1;

#if DEBUG_LOG

	PPM("BEFORE ICapture Process");

#endif

    status = (capture_status_t) iobj->lib.Process(iobj->lib_private, &iobj->proc);

    if( ICAPTURE_FAIL == status){
        LOGE("ICapture Process failed");
        goto fail_process;
    }

#if DEBUG_LOG

    else {
        PPM("ICapture process OK");
    }
    
    LOGD("iobj->proc.out_img_w = %d = 0x%x iobj->proc.out_img_h=%u = 0x%x", (int)iobj->proc.out_img_w,(int)iobj->proc.out_img_w, (int)iobj->proc.out_img_h,(int)iobj->proc.out_img_h);
    
#endif

    pixelFormat = PIX_YUV422I;

//block until snapshot is ready
    fd_set descriptorSet;
    int max_fd;
    unsigned int snapshotReadyMessage;

	max_fd = snapshotReadyPipe[0] + 1;

	FD_ZERO(&descriptorSet);
	FD_SET(snapshotReadyPipe[0], &descriptorSet);

#ifdef DEBUG_LOG

		LOGD("Waiting on SnapshotThread ready message");

#endif

    err = select(max_fd,  &descriptorSet, NULL, NULL, NULL);
	if (err < 1) {
		LOGE("Error in select");
	}

	if(FD_ISSET(snapshotReadyPipe[0], &descriptorSet))
		read(snapshotReadyPipe[0], &snapshotReadyMessage, sizeof(snapshotReadyMessage));
//

#if PPM_INSTRUMENTATION

	PPM("SENDING MESSAGE TO PROCESSING THREAD");

#endif
	
	procMessage[0] = PROC_THREAD_PROCESS;
	procMessage[1] = iobj->proc.out_img_w;
	procMessage[2] = iobj->proc.out_img_h;
	procMessage[3] = image_width;
	procMessage[4] = image_height;
	procMessage[5] = pixelFormat;			
    procMessage[6] = iobj->proc.eenf.ee_q;
    procMessage[7] = iobj->proc.eenf.ew_ts;
    procMessage[8] = iobj->proc.eenf.es_ts; 
    procMessage[9] = iobj->proc.eenf.luma_nf;
    procMessage[10] = iobj->proc.eenf.chroma_nf;
    procMessage[11] = (unsigned int) mPictureHeap.get();
    procMessage[12] = (unsigned int) yuv_buffer;
    procMessage[13] = offset;
    procMessage[14] = yuv_len;
    procMessage[15] = rotation;
    procMessage[16] = mZoomTarget;
    procMessage[17] = mippMode;
    procMessage[18] = mIPPToEnable;
    procMessage[19] = quality;
    procMessage[20] = (unsigned int) mJpegPictureCallback;
    procMessage[21] = (unsigned int) mRawPictureCallback;
    procMessage[22] = (unsigned int) mPictureCallbackCookie;

    write(procPipe[1], &procMessage, sizeof(procMessage));

    mIPPToEnable = false; // reset ipp enable after sending to proc thread

#ifdef DEBUG_LOG

    LOGD("\n\n\n PICTURE NUMBER =%d\n\n\n",++pictureNumber);

#endif
    
    if( mShutterCallback ) {

#ifdef DEBUG_LOG

        PPM("SENDING MESSAGE TO SHUTTER THREAD");

#endif

        shutterMessage[0] = SHUTTER_THREAD_CALL;
        shutterMessage[1] = (unsigned int) mShutterCallback;
        shutterMessage[2] = (unsigned int) mPictureCallbackCookie;
        write(shutterPipe[1], &shutterMessage, sizeof(shutterMessage));
    }

#ifdef DEBUG_LOG

    PPM("IMAGE CAPTURED");

#endif

    if ( mRawPictureCallback ) {

#ifdef DEBUG_LOG

        PPM("SENDING MESSAGE TO RAW THREAD");

#endif

        rawMessage[0] = SHUTTER_THREAD_CALL;
        rawMessage[1] = (unsigned int) mRawPictureCallback;
        rawMessage[2] = (unsigned int) mPictureCallbackCookie;
        rawMessage[3] = (unsigned int) NULL;
        write(rawPipe[1], &rawMessage, sizeof(rawMessage));
    }

#ifdef DEBUG_LOG

	LOG_FUNCTION_NAME_EXIT

#endif

	return 0;

fail_config :
fail_process:

    return -1;   
}

#endif

void CameraHal::snapshotThread()
{
    fd_set descriptorSet;
    int max_fd;
    int err, status;
    unsigned int snapshotMessage[5], snapshotReadyMessage;
    int image_width, image_height, pixelFormat, preview_width, preview_height;
    mapping_data_t* data;
	overlay_buffer_t overlaybuffer;
    void *yuv_buffer, *snapshot_buffer;
    int ZoomTarget;

    LOG_FUNCTION_NAME

    pixelFormat = PIX_YUV422I;
	max_fd = snapshotPipe[0] + 1;

	FD_ZERO(&descriptorSet);
	FD_SET(snapshotPipe[0], &descriptorSet);

    while(1) {
        err = select(max_fd,  &descriptorSet, NULL, NULL, NULL);

#ifdef DEBUG_LOG

		LOGD("SNAPSHOT THREAD SELECT RECEIVED A MESSAGE\n");

#endif

		if (err < 1) {
			LOGE("Snapshot: Error in select");
		}

		if(FD_ISSET(snapshotPipe[0], &descriptorSet)){

			read(snapshotPipe[0], &snapshotMessage, sizeof(snapshotMessage));

			if(snapshotMessage[0] == SNAPSHOT_THREAD_START){

#ifdef DEBUG_LOG

				LOGD("SNAPSHOT_THREAD_START RECEIVED\n");

#endif

                yuv_buffer = (void *) snapshotMessage[1];
				image_width = snapshotMessage[2];
				image_height = snapshotMessage[3];
				ZoomTarget = snapshotMessage[4];

	            mParameters.getPreviewSize(&preview_width, &preview_height);

	            data = (mapping_data_t*)mOverlay->getBufferAddress( (void*)(lastOverlayBufferDQ) );

                if ( data == NULL ) {
                    LOGE(" getBufferAddress returned NULL");
                }

	            snapshot_buffer = (void*)data->ptr;

#if PPM_INSTRUMENTATION

	            PPM("Before vpp downscales:");

#endif

                status = scale_process(yuv_buffer, image_width, image_height,
                         snapshot_buffer, preview_width, preview_height, 0, PIX_YUV422I, ZoomTarget);

#ifdef DEBUG_LOG

	            PPM("After vpp downscales:");

	            if( status )
	                LOGE("scale_process() failed");
	            else
	                LOGD("scale_process() OK");

#endif

#if PPM_INSTRUMENTATION

	            PPM("Shot to Snapshot", &ppm_receiveCmdToTakePicture);

#endif

	            status = mOverlay->queueBuffer((void*)(lastOverlayBufferDQ));
                if (status) {
		            LOGE("mOverlay->queueBuffer() failed!!!!");
                } else {
                    buffers_queued_to_dss[lastOverlayBufferDQ]=1;
                    nOverlayBuffersQueued++;
                }

                status = mOverlay->dequeueBuffer(&overlaybuffer);
                if (status) {
                    LOGE("mOverlay->dequeueBuffer() failed!!!!");
                } else {
                    nOverlayBuffersQueued--;
                    buffers_queued_to_dss[(int)overlaybuffer] = 0;
                    lastOverlayBufferDQ = (int)overlaybuffer;
                }

                write(snapshotReadyPipe[1], &snapshotReadyMessage, sizeof(snapshotReadyMessage));

		    } else if (snapshotMessage[0] == SNAPSHOT_THREAD_EXIT) {
				LOGD("SNAPSHOT_THREAD_EXIT RECEIVED");

				break;
		    }
        }
    }

    LOG_FUNCTION_NAME_EXIT
}

void CameraHal::rawThread()
{
    LOG_FUNCTION_NAME

    fd_set descriptorSet;
    int max_fd;
    int err;
    unsigned int rawMessage[RAW_THREAD_NUM_ARGS];
    raw_callback RawCallback;
    void *PictureCallbackCookie;
    sp<MemoryBase> rawData;
    
	max_fd = rawPipe[0] + 1;

	FD_ZERO(&descriptorSet);
	FD_SET(rawPipe[0], &descriptorSet);
    
    while(1) {
        err = select(max_fd,  &descriptorSet, NULL, NULL, NULL);

#ifdef DEBUG_LOG

		LOGD("RAW THREAD SELECT RECEIVED A MESSAGE\n");

#endif

		if (err < 1) {
			LOGE("Raw: Error in select");
		}

		if(FD_ISSET(rawPipe[0], &descriptorSet)){

			read(rawPipe[0], &rawMessage, sizeof(rawMessage));
			
			if(rawMessage[0] == RAW_THREAD_CALL){

#ifdef DEBUG_LOG

				LOGD("RAW_THREAD_CALL RECEIVED\n");

#endif

				RawCallback = (raw_callback) rawMessage[1];
				PictureCallbackCookie = (void *) rawMessage[2];
				rawData = (MemoryBase *) rawMessage[3];

				RawCallback(rawData, PictureCallbackCookie);

#ifdef DEBUG_LOG

				PPM("RAW CALLBACK CALLED");

#endif

		    } else if (rawMessage[0] == RAW_THREAD_EXIT) {
				LOGD("RAW_THREAD_EXIT RECEIVED");
				
				break;
		    }
        }
    }
    
    LOG_FUNCTION_NAME_EXIT
}

void CameraHal::shutterThread()
{
    LOG_FUNCTION_NAME

    fd_set descriptorSet;
    int max_fd;
    int err;
    unsigned int shutterMessage[3];
    shutter_callback ShutterCallback;
    void *PictureCallbackCookie;
    
	max_fd = shutterPipe[0] + 1;

	FD_ZERO(&descriptorSet);
	FD_SET(shutterPipe[0], &descriptorSet);
    
    while(1) {
        err = select(max_fd,  &descriptorSet, NULL, NULL, NULL);

#ifdef DEBUG_LOG

		LOGD("SHUTTER THREAD SELECT RECEIVED A MESSAGE\n");

#endif

		if (err < 1) {
			LOGE("Shutter: Error in select");
		}

		if(FD_ISSET(shutterPipe[0], &descriptorSet)){

			read(shutterPipe[0], &shutterMessage, sizeof(shutterMessage));
			
			if(shutterMessage[0] == SHUTTER_THREAD_CALL){

#ifdef DEBUG_LOG

				LOGD("SHUTTER_THREAD_CALL_RECEIVED\n");

#endif

				ShutterCallback = (shutter_callback) shutterMessage[1];
				PictureCallbackCookie = (void *) shutterMessage[2];
				
				ShutterCallback(PictureCallbackCookie);

#ifdef DEBUG_LOG

				PPM("CALLED SHUTTER CALLBACK");

#endif

		    } else if (shutterMessage[0] == SHUTTER_THREAD_EXIT) {
				LOGD("SHUTTER_THREAD_EXIT RECEIVED");
				
				break;
		    }
        }
    }
    
    LOG_FUNCTION_NAME_EXIT
}

void CameraHal::procThread()
{
	LOG_FUNCTION_NAME

	int status;
    int capture_width, capture_height, image_width, image_height;
    unsigned short ipp_ee_q, ipp_ew_ts, ipp_es_ts, ipp_luma_nf, ipp_chroma_nf;
	fd_set descriptorSet;
	int max_fd;
	int err;
	int pixelFormat;
	unsigned int procMessage [PROC_THREAD_NUM_ARGS];
	int jpegQuality, jpegSize, size, base, offset, yuv_offset, yuv_len, image_rotation, image_zoom, ippMode;
    bool ipp_to_enable;
	sp<MemoryHeapBase> JPEGPictureHeap, PictureHeap;
    sp<MemoryBase> JPEGPictureMemBase;
    raw_callback RawPictureCallback;
    jpeg_callback JpegPictureCallback;
    void *yuv_buffer, *outBuffer, *PictureCallbackCookie;
    bool switchBuffer = false;
	
	//debug
	sp<MemoryHeapBase> tmpHeap;
	int tmpLength;
	void *tmpBuffer;

	max_fd = procPipe[0] + 1;

	FD_ZERO(&descriptorSet);
	FD_SET(procPipe[0], &descriptorSet);

    mJPEGLength  = PICTURE_WIDTH*PICTURE_HEIGHT + ((2*PAGE) - 1);
    mJPEGLength &= ~((2*PAGE) - 1);
    mJPEGLength  += 2*PAGE;
    mJPEGPictureHeap = new MemoryHeapBase(mJPEGLength);

    base = (unsigned long) mJPEGPictureHeap->getBase();
    base = (base + 0xfff) & 0xfffff000;
    mJPEGOffset = base - (unsigned long) mJPEGPictureHeap->getBase();
    mJPEGBuffer = (void *) base;

    //temporary
    tmpLength  = PICTURE_WIDTH*PICTURE_HEIGHT*2 + ((2*PAGE) - 1);
    tmpLength &= ~((2*PAGE) - 1);
    tmpLength  += 2*PAGE;
    tmpHeap = new MemoryHeapBase(tmpLength);

    base = (unsigned long) tmpHeap->getBase();
    base = (base + 0xfff) & 0xfffff000;
    tmpBuffer = (void *) base;

	while(1){

		err = select(max_fd,  &descriptorSet, NULL, NULL, NULL);

#ifdef DEBUG_LOG

		LOGD("PROCESSING THREAD SELECT RECEIVED A MESSAGE\n");

#endif

		if (err < 1) {
			LOGE("Proc: Error in select");
		}

		if(FD_ISSET(procPipe[0], &descriptorSet)){

			read(procPipe[0], &procMessage, sizeof(procMessage));
			
			if(procMessage[0] == PROC_THREAD_PROCESS){

#ifdef DEBUG_LOG

				LOGD("PROC_THREAD_PROCESS_RECEIVED\n");
				
#endif

				capture_width = procMessage[1];
				capture_height = procMessage[2];
				image_width = procMessage[3];
				image_height = procMessage[4];
				pixelFormat = procMessage[5];
				ipp_ee_q = procMessage[6];
				ipp_ew_ts = procMessage[7];
				ipp_es_ts = procMessage[8];
				ipp_luma_nf = procMessage[9];
				ipp_chroma_nf = procMessage[10];
				PictureHeap = (MemoryHeapBase *) procMessage[11];
				yuv_buffer = (void *) procMessage[12];
				yuv_offset =  procMessage[13];
				yuv_len = procMessage[14];
				image_rotation = procMessage[15];
				image_zoom = procMessage[16];
				ippMode = procMessage[17];
                ipp_to_enable = procMessage[18];
				jpegQuality = procMessage[19];
				JpegPictureCallback = (jpeg_callback) procMessage[20];
				RawPictureCallback = (raw_callback) procMessage[21];
				PictureCallbackCookie = (void *) procMessage[22];

                jpegSize = mJPEGLength;
                JPEGPictureHeap = mJPEGPictureHeap;
                outBuffer = mJPEGBuffer;
                offset = mJPEGOffset;

#if RESIZER

                if( (image_width != capture_width) || (image_height != capture_height) || (image_rotation != 0) || (image_zoom != 1) ) {

#ifdef DEBUG_LOG

                    LOGI("Process VPP ( %d x %d -> %d x %d ) - starting", capture_width, capture_height, (int) image_width, (int) image_height);

#endif

		            err = scale_process(yuv_buffer, capture_width, capture_height, tmpBuffer, image_width, image_height, image_rotation, pixelFormat, image_zoom);

#ifdef DEBUG_LOG

		            if( err) {
                        LOGE("Process Resizer VPP - failed");
                    } else {
	                	LOGE("Process Resizer VPP - OK");
                    }

#endif

                    switchBuffer = true;

                    if( (rotation == 90) || (rotation == 270) ) {
                        int tmp = image_width;
                        image_width = image_height;
                        image_height = tmp;
                    }

                }
#else
                image_width = capture_width;
                image_height = capture_height;
#endif //RESIZER	

#ifdef IMAGE_PROCESSING_PIPELINE  	
	            if(ippMode == -1 ){
		            ippMode = IPP_EdgeEnhancement_Mode;
	            }

#ifdef DEBUG_LOG

	            LOGD("IPPmode=%d",ippMode);
	            if(ippMode == IPP_CromaSupression_Mode){
		            LOGD("IPP_CromaSupression_Mode");
	            }
	            else if(ippMode == IPP_EdgeEnhancement_Mode){
		            LOGD("IPP_EdgeEnhancement_Mode");
	            }
	            else if(ippMode == IPP_Disabled_Mode){
		            LOGD("IPP_Disabled_Mode");
	            }

#endif

	            if(ippMode){

                    if(ippMode != IPP_CromaSupression_Mode && ippMode != IPP_EdgeEnhancement_Mode)
		                LOGE("ERROR ippMode unsupported");

                    if(ipp_to_enable)
                    {
#ifdef DEBUG_LOG

		                PPM("Before init IPP");

#endif

	    	            if(InitIPP(image_width, image_height, pixelFormat))
        		            LOGE("ERROR InitIPP() failed");
#ifdef DEBUG_LOG

		                PPM("After IPP Init");

#endif
                    }

		            err = PopulateArgsIPP(image_width, image_height, pixelFormat);
		            if( err )
			            LOGE("ERROR PopulateArgsIPP() failed");	   

#ifdef DEBUG_LOG

		            PPM("BEFORE IPP Process Buffer");
                    LOGD("Calling ProcessBufferIPP(buffer=%p , len=0x%x)", yuv_buffer, yuv_len);

#endif

                    if( switchBuffer) {
		                err = ProcessBufferIPP(tmpBuffer, tmpLength,
		                            pixelFormat,
				                    ipp_ee_q,
				                    ipp_ew_ts,
				                    ipp_es_ts,
				                    ipp_luma_nf,
				                    ipp_chroma_nf);
				    } else {
		                err = ProcessBufferIPP(yuv_buffer, yuv_len,
		                            pixelFormat,
				                    ipp_ee_q,
				                    ipp_ew_ts,
				                    ipp_es_ts, 
				                    ipp_luma_nf,
				                    ipp_chroma_nf);
				    }
		            if( err )
			            LOGE("ERROR ProcessBufferIPP() failed");

                   	if(!(pIPP.ippconfig.isINPLACE)){ 
		                yuv_buffer = pIPP.pIppOutputBuffer;
	                }	                
	                pixelFormat = PIX_YUV420P;
	            }
    
#endif

#if JPEG
                err = 0;    

#ifdef DEBUG_LOG

	            PPM("BEFORE JPEG Encode Image");
	            	
	            LOGD(" outbuffer = %p, jpegSize = %d, yuv_buffer = %p, yuv_len = %d, image_width = %d, image_height = %d, quality = %d, ippMode =%d", outBuffer , jpegSize, tmpBuffer/*yuv_buffer*/, mJPEGLength/*yuv_len*/, image_width, image_height, jpegQuality, ippMode);

#endif

                if( switchBuffer ) {
                    if (!( jpegEncoder->encodeImage((uint8_t *)outBuffer , jpegSize, tmpBuffer, tmpLength,
                                                 image_width, image_height, jpegQuality, pixelFormat)))
                    {
                        err = -1;
                        LOGE("JPEG Encoding failed");
                    }
                } else {
                    if (!( jpegEncoder->encodeImage((uint8_t *)outBuffer , jpegSize, yuv_buffer, yuv_len,
                                                 image_width, image_height, jpegQuality, pixelFormat)))
                    {
                        err = -1;
                        LOGE("JPEG Encoding failed");
                    }
                }
#ifdef DEBUG_LOG

                PPM("AFTER JPEG Encode Image");
                
#endif

                JPEGPictureMemBase = new MemoryBase(JPEGPictureHeap, offset, jpegEncoder->jpegSize);
#endif

#if PPM_INSTRUMENTATION

	            PPM("Shot to Save", &ppm_receiveCmdToTakePicture);

#endif
                if(JpegPictureCallback) {

#if JPEG

		            JpegPictureCallback(JPEGPictureMemBase, PictureCallbackCookie); 

#else

		            JpegPictureCallback(NULL, PictureCallbackCookie); 

#endif

                }

#ifdef DEBUG_LOG

                LOGD("jpegEncoder->jpegSize=%d jpegSize=%d", jpegEncoder->jpegSize, jpegSize);
   
#endif

                JPEGPictureMemBase.clear();
                
                PictureHeap->dispose();
                PictureHeap.clear();

                switchBuffer = false;

#ifdef OPP_OPTIMIZATION

                    if ( RMProxy_ReleaseBoost() != OMX_ErrorNone ) {
                        LOGE("OPP Release Boost failed");
                    } else {
                        LOGE("OPP Release Boost success");
                    }

#endif

			} else if( procMessage[0] == PROC_THREAD_EXIT ) {
				LOGD("PROC_THREAD_EXIT_RECEIVED");
				
				mJPEGPictureHeap->dispose();
				mJPEGPictureHeap.clear();
				tmpHeap->dispose();
				tmpHeap.clear();
				
				break;
			}
		}
	}

    JPEGPictureHeap.clear();

	LOG_FUNCTION_NAME_EXIT
}

#ifdef ICAP_EXPERIMENTAL

int CameraHal::allocatePictureBuffer(size_t length)
{
    int base;

    mPictureLength  = length + ((2*PAGE) - 1);
    mPictureLength &= ~((2*PAGE) - 1);
    mPictureLength  += 2*PAGE ;
    mPictureHeap = new MemoryHeapBase(mPictureLength);

    base = (unsigned long) mPictureHeap->getBase();
    base = (base + 0xfff) & 0xfffff000;
    mPictureOffset = base - (unsigned long) mPictureHeap->getBase();
    mYuvBuffer = (uint8_t *) base;

    return 0;
}

#else

int CameraHal::allocatePictureBuffer(int width, int height)
{
    int base;

    mPictureLength  = width*height*2 + ((2*PAGE) - 1) + 10*PAGE;
    mPictureLength &= ~((2*PAGE) - 1);
    mPictureLength  += 2*PAGE;
    mPictureHeap = new MemoryHeapBase(mPictureLength);

    base = (unsigned long) mPictureHeap->getBase();
    base = (base + 0xfff) & 0xfffff000;
    mPictureOffset = base - (unsigned long) mPictureHeap->getBase();
    mYuvBuffer = (uint8_t *) base;

    return 0;
}

#endif

int CameraHal::ICaptureCreate(void)
{
    int res;
    overlay_buffer_t overlaybuffer;
    int image_width, image_height;

    LOG_FUNCTION_NAME
    
#ifdef ICAP
    iobj = (libtest_obj *) malloc( sizeof( *iobj));
    if( NULL == iobj) {
        LOGE("libtest_obj malloc failed");
        goto exit;
    }
#endif

    mParameters.getPictureSize(&image_width, &image_height);
    LOGD("ICaptureCreate: Picture Size %d x %d", image_width, image_height);
#ifdef ICAP
    memset(iobj, 0 , sizeof(*iobj));

    iobj->lib.lib_handle = dlopen(LIBICAPTURE_NAME, RTLD_LAZY);
    if( NULL == iobj->lib.lib_handle){
        LOGE("Can not open ICapture Library: %s", dlerror());
        goto exit;
    }

    if( res >= 0){error:
        iobj->lib.Create = ( int (*) (void **, int) ) dlsym(iobj->lib.lib_handle, "capture_create");
        if(NULL == iobj->lib.Create){
            res = -1;
        }
    }

    if( res >= 0){
        iobj->lib.Delete = ( int (*) (void *) ) dlsym(iobj->lib.lib_handle, "capture_delete");
        if( NULL == iobj->lib.Delete){
            res = -1;
        }
    }

    if( res >= 0){
        iobj->lib.Config = ( int (*) (void *, capture_config_t *) ) dlsym(iobj->lib.lib_handle, "capture_config");
        if( NULL == iobj->lib.Config){
            res = -1;
        }
    }

    if( res >= 0){
        iobj->lib.Process = ( int (*) (void *, capture_process_t *) ) dlsym(iobj->lib.lib_handle, "capture_process");
        if( NULL == iobj->lib.Process){
            res = -1;
        }
    }

    res = (capture_status_t) iobj->lib.Create(&iobj->lib_private, camera_device);
    if( ( ICAPTURE_FAIL == res) || NULL == iobj->lib_private){
        LOGE("ICapture Create function failed");
        goto fail_icapture;
    }
    LOGD("ICapture create OK");

#endif

#ifdef HARDWARE_OMX

#ifdef VPP
    if ( scale_init(PICTURE_WIDTH, PICTURE_HEIGHT, PICTURE_WIDTH, PICTURE_HEIGHT, PIX_YUV422I, PIX_YUV422I) < 0 ) {
        LOGE("scale_init failed()");
        
        return -1;
    } else {
        isStart_VPP = true;
    }
#endif
#ifdef IMAGE_PROCESSING_PIPELINE

#ifdef IMAGE_PROCESSING_PIPELINE
	mippMode = IPP_Disabled_Mode;
#endif

#endif
#if JPEG
    jpegEncoder = new JpegEncoder;
    
    if( NULL != jpegEncoder )
        isStart_JPEG = true;
#endif
#endif

    LOG_FUNCTION_NAME_EXIT
    return res;

fail_jpeg_buffer:
fail_yuv_buffer:
fail_init:

#ifdef HARDWARE_OMX
#if JPEG
    delete jpegEncoder;
#endif
#endif    

#ifdef HARDWARE_OMX

#endif

#ifdef ICAP
    iobj->lib.Delete(&iobj->lib_private);
#endif

fail_icapture:
exit:
    return -1;
}

int CameraHal::ICaptureDestroy(void)
{
    int err;
#ifdef HARDWARE_OMX
#ifdef VPP
    if( isStart_VPP )
        scale_deinit();
#endif

#if JPEG
    if( isStart_JPEG )
        delete jpegEncoder;
#endif
#endif

#ifdef ICAP
    if (iobj->lib_private) {
        err = (capture_status_t) iobj->lib.Delete(iobj->lib_private);
        if(ICAPTURE_FAIL == err){
            LOGE("ICapture Delete failed");
        } else {
            LOGD("ICapture delete OK");
        }
    }

    iobj->lib_private = NULL;
 
    dlclose(iobj->lib.lib_handle);
    free(iobj);
    iobj = NULL;
#endif

    return 0;
}

status_t CameraHal::setOverlay(const sp<Overlay> &overlay)
{
    Mutex::Autolock lock(mLock);
    int w,h;

    LOGD("CameraHal setOverlay/1/%08lx/%08lx", (long unsigned int)overlay.get(), (long unsigned int)mOverlay.get());
    // De-alloc any stale data
    if ( mOverlay.get() != NULL )
    {
        LOGD("Destroying current overlay");
        
        int buffer_count = mOverlay->getBufferCount();
        for(int i =0; i < buffer_count ; i++){
            buffers_queued_to_dss[i] = 0;
        }

        mOverlay->destroy();
        nOverlayBuffersQueued = 0;
    }

    mOverlay = overlay;
    if (mOverlay == NULL)
    {
        LOGE("Trying to set overlay, but overlay is null!, line:%d",__LINE__);
        return NO_ERROR;
    }

    mParameters.getPreviewSize(&w, &h);

    if ((w == RES_720P) || (h == RES_720P))
    {
        mOverlay->setAttributes(CACHEABLE_BUFFERS, 1);
        mOverlay->setAttributes(MAINTAIN_COHERENCY, 0);
        mOverlay->resizeInput(w, h);
    }

    // Restart the preview (Only for Overlay Case)
    LOGD("Restart the preview ");
    startPreview(NULL,NULL);

	LOG_FUNCTION_NAME_EXIT

    return NO_ERROR;
}

status_t CameraHal::startPreview(preview_callback cb, void* user)
{
    LOG_FUNCTION_NAME

    if (mOverlay == NULL)
    {
        LOGE("WARNING: Trying to set overlay, but overlay is null!, line:%d",__LINE__);
        return NO_ERROR;
    }

    if (mPreviewRunning){
        LOGE("WARNING: Trying to startPreview but already started!, line:%d",__LINE__);
        return NO_ERROR;
    }

    Message msg;
    msg.command = PREVIEW_START;
    msg.arg1 = (void*)cb;
    msg.arg2 = (void*)user;
    previewThreadCommandQ.put(&msg);
    previewThreadAckQ.get(&msg);

    LOG_FUNCTION_NAME_EXIT
    return msg.command == PREVIEW_ACK ? NO_ERROR : INVALID_OPERATION;
}

void CameraHal::stopPreview()
{
    LOG_FUNCTION_NAME

    Message msg;
    msg.command = PREVIEW_STOP;
    previewThreadCommandQ.put(&msg);
    previewThreadAckQ.get(&msg);
}

status_t CameraHal::autoFocus(autofocus_callback af_cb,
                                       void *user)
{
    LOG_FUNCTION_NAME

    Message msg;
    msg.command = PREVIEW_AF_START;
    msg.arg1 = (void*)af_cb;
    msg.arg2 = (void*)user;
    previewThreadCommandQ.put(&msg);
    previewThreadAckQ.get(&msg);

   LOG_FUNCTION_NAME_EXIT
    return msg.command == PREVIEW_ACK ? NO_ERROR : INVALID_OPERATION;
}

bool CameraHal::previewEnabled()
{
    return mPreviewRunning;
}

status_t CameraHal::startRecording(recording_callback cb, void* user)
{
    LOG_FUNCTION_NAME
    int w,h;
    int i = 0;

    for(i = 0; i < VIDEO_FRAME_COUNT_MAX; i++)
    {
        mVideoBufferUsing[i] = 0;
    }

    mParameters.getPreviewSize(&w, &h);

    // Just for the same size case
    mRecordingFrameSize = w * h * 2;

    overlay_handle_t overlayhandle = mOverlay->getHandleRef();

    overlay_true_handle_t true_handle;

    if ( overlayhandle == NULL ) {
        LOGD("overlayhandle is received as NULL. ");
        return UNKNOWN_ERROR;
    }

    memcpy(&true_handle,overlayhandle,sizeof(overlay_true_handle_t));

    int overlayfd = true_handle.ctl_fd;

    LOGD("#Overlay driver FD:%d ",overlayfd);

    mVideoBufferCount =  mOverlay->getBufferCount();
#if 0
    for(i = 0; i < mVideoBufferCount; i++)
    {
        mVideoBufferPtr[i] = (unsigned long)mOverlay->getBufferAddress((void*)i);
        LOGD("mVideoBufferPtr[%d] = 0x%x", i,mVideoBufferPtr[i]);
    }
#endif
    if(cb)
    {
        LOGD("Clear the old memory ");
        mVideoHeap.clear();
        for( i = 0; i < mVideoBufferCount; i++)
        {
            mVideoHeaps[i].clear();
            mVideoBuffer[i].clear();
        }
        LOGD("Mmap the video Memory %d", mPreviewFrameSize);

#if USE_NEW_OVERLAY
        for(i = 0; i < mVideoBufferCount; i++)
        {
            mapping_data_t* data = (mapping_data_t*) mOverlay->getBufferAddress((void*)i);
            mVideoHeaps[i]  = new MemoryHeapBase(data->fd,mPreviewFrameSize, 0, data->offset);
            mVideoBuffer[i] = new MemoryBase(mVideoHeaps[i], 0, mRecordingFrameSize);
            mPreviewBlocks[i] = data->ptr;
            LOGD("mVideoHeaps[%d]: ID:%d,Base:[%p],size:%d", i, mVideoHeaps[i]->getHeapID(),
                                       mVideoHeaps[i]->getBase() ,mVideoHeaps[i]->getSize());
            LOGD("mVideoBuffer[%d]: Pointer[%p]", i, mVideoBuffer[i]->pointer());
        }
#else

#if USE_MEMCOPY_FOR_VIDEO_FRAME
        mVideoHeap = new MemoryHeapBase(mPreviewFrameSize * mVideoBufferCount);
        LOGD("mVideoHeap ID:%d , Base:[%x],size:%d", mVideoHeap->getHeapID(),
                                       mVideoHeap->getBase(),mVideoHeap->getSize());
#else
        mVideoHeap = new MemoryHeapBase(overlayfd,mPreviewFrameSize * mVideoBufferCount);
#endif
        LOGD("mVideoHeap ID:%d , Base:[%x],size:%d", mVideoHeap->getHeapID(),
                                       mVideoHeap->getBase(),mVideoHeap->getSize());

        for(i = 0; i < mVideoBufferCount; i++)
        {
            LOGD("Init Video Buffer:%d ",i);
            mVideoBuffer[i] = new MemoryBase(mVideoHeap, mPreviewFrameSize*i, mRecordingFrameSize);
            LOGD("pointer:[%x],size:%d,offset:%d", mVideoBuffer[i]->pointer(),mVideoBuffer[i]->size(),mVideoBuffer[i]->offset());
        }
#endif
    }
    mRecordingLock.lock();
    mRecordingCallback = cb;
    mRecordingCallbackCookie = user;
    mRecordingLock.unlock();
    return NO_ERROR;
}

void CameraHal::stopRecording()
{
    LOG_FUNCTION_NAME
    mRecordingLock.lock();
    mRecordingCallback = NULL;
    mRecordingCallbackCookie = NULL;


    mRecordingLock.unlock();
}

bool CameraHal::recordingEnabled()
{
    LOG_FUNCTION_NAME
    return mRecordingCallback !=0;
}

static void debugShowFPS()
{
    static int mFrameCount = 0;
    static int mLastFrameCount = 0;
    static nsecs_t mLastFpsTime = 0;
    static float mFps = 0;
    mFrameCount++;
    if (!(mFrameCount & 0x1F)) {
        nsecs_t now = systemTime();
        nsecs_t diff = now - mLastFpsTime;
        mFps =  ((mFrameCount - mLastFrameCount) * float(s2ns(1))) / diff;
        mLastFpsTime = now;
        mLastFrameCount = mFrameCount;
        LOGD("####### [%d] Frames, %f FPS", mFrameCount, mFps);
    }
 }

void CameraHal::releaseRecordingFrame(const sp<IMemory>& mem)
{
    //LOG_FUNCTION_NAME
    ssize_t offset;
    size_t  size;
    int index;
    int time = 0;
#if USE_NEW_OVERLAY

    for(index = 0; index <mVideoBufferCount; index ++){
//        LOGD("mVideoBuffer[%d]->pointer() %x",index,mVideoBuffer[index]->pointer());
        if(mem->pointer() == mVideoBuffer[index]->pointer()) {
            break;
        }
    }

//    LOGD("index = %d  pointer=0x%x",index,mem->pointer());

#else
    offset = mem->offset();
    size   = mem->size();
    index = offset / size;
#endif

    mRecordingFrameCount++;

    debugShowFPS();
#if USE_MEMCOPY_FOR_VIDEO_FRAME
    mVideoBufferUsing[index] = 0;
#else
    if (ioctl(camera_device, VIDIOC_QBUF, &v4l2_cam_buffer[index]) < 0) {
        LOGE("VIDIOC_QBUF Failed, index [%d] line=%d",index,__LINE__);
    } else {
        //LOGE("releaseRecordingFrame index##[%d]",index);
	    nCameraBuffersQueued++;
	}

#endif
    return;
}

sp<IMemoryHeap>  CameraHal::getRawHeap() const
{
    return mPictureHeap;
}

status_t CameraHal::takePicture(shutter_callback shutter_cb,
                                         raw_callback raw_cb,
                                         jpeg_callback jpeg_cb,
                                         void* user)
{
    LOG_FUNCTION_NAME

    Message msg;
    msg.command = PREVIEW_CAPTURE;
    msg.arg1    = (void*)shutter_cb;
    msg.arg2    = (void*)raw_cb;
    msg.arg3    = (void*)jpeg_cb;
    msg.arg4    = user;
    previewThreadCommandQ.put(&msg);
    previewThreadAckQ.get(&msg);

#ifdef DEBUG_LOG

    LOG_FUNCTION_NAME_EXIT

#endif

    return NO_ERROR;
}

status_t CameraHal::cancelPicture(bool cancel_shutter,
                                           bool cancel_raw,
                                           bool cancel_jpeg)
{
    LOG_FUNCTION_NAME

    mRawPictureCallback = NULL;
    mJpegPictureCallback = NULL;
    mPictureCallbackCookie = NULL;

    LOGE("Callbacks set to null");
    return -1;
}

int CameraHal::validateSize(int w, int h)
{
    if ((w < MIN_WIDTH) || (h < MIN_HEIGHT)){
        return false;
    }
    return true;
}

status_t CameraHal::setParameters(const CameraParameters &params)
{
    LOG_FUNCTION_NAME

    int w, h;
    int framerate;
    int w_orig, h_orig;
    int iso, af, mode, zoom, wb, exposure, scene;
    int effects, compensation, saturation, sharpness;
    int contrast, brightness, flash, caf;
	int error;
	int base;
    Message msg;

    Mutex::Autolock lock(mLock);
      
    LOGD("PreviewFormat %s", params.getPreviewFormat());

    if ( params.getPreviewFormat() != NULL ) {
        if (strcmp(params.getPreviewFormat(), "yuv422i") != 0) {
            LOGE("Only yuv422i preview is supported");
            return -1;
        }
    }
    
    LOGD("PictureFormat %s", params.getPictureFormat());
    if ( params.getPictureFormat() != NULL ) {
        if (strcmp(params.getPictureFormat(), "jpeg") != 0) {
            LOGE("Only jpeg still pictures are supported");
            return -1;
        }
    }

    params.getPreviewSize(&w, &h);
    if (!validateSize(w, h)) {
        LOGE("Preview size not supported");
        return -1;
    }
    LOGD("PreviewResolution by App %d x %d", w, h);

    params.getPictureSize(&w, &h);
    if (!validateSize(w, h)) {
        LOGE("Picture size not supported");
        return -1;
    }
    LOGD("Picture Size by App %d x %d", w, h);

    framerate = params.getPreviewFrameRate();
    LOGD("FRAMERATE %d", framerate);

    mParameters.getPictureSize(&w_orig, &h_orig);

    mParameters = params;

#ifdef IMAGE_PROCESSING_PIPELINE	
    if((mippMode != mParameters.getInt("ippMode")) ||
	            (w != w_orig) ||
	            (h != h_orig) )
    {
        mippMode=mParameters.getInt("ippMode");
        LOGD("mippMode=%d",mippMode);

        if(pIPP.hIPP != NULL){
            LOGD("pIPP.hIPP=%p", pIPP.hIPP);
            if(DeInitIPP()) // deinit here to save time
                LOGE("ERROR DeInitIPP() failed");
            pIPP.hIPP = NULL;
        }
        mIPPToEnable = true;
    }
#endif //IMAGE_PROCESSING_PIPELINE

	mParameters.getPictureSize(&w, &h);
	LOGD("Picture Size by CamHal %d x %d", w, h);
	
	mParameters.getPreviewSize(&w, &h);
	LOGD("Preview Resolution by CamHal %d x %d", w, h);

    quality = params.getInt("jpeg-quality");
    if ( ( quality < 0 ) || (quality > 100) ){
        quality = 100;
    } 

    mZoomTarget = mParameters.getInt("zoom");
    if( (mZoomTarget < 1) || (mZoomTarget > 7) ){
        mZoomTarget = 1;
    }

#ifdef FW3A

    if ( NULL != fobj ){
        iso = mParameters.getInt("iso");
        af = mParameters.getInt("af");
        mcapture_mode = mParameters.getInt("mode");        
        wb = mParameters.getInt("wb");
        exposure = mParameters.getInt("exposure");
        scene = mParameters.getInt("scene");
        effects = mParameters.getInt("effects");
        compensation = mParameters.getInt("compensation");
        saturation = mParameters.getInt("saturation");
        sharpness = mParameters.getInt("sharpness");
        contrast = mParameters.getInt("contrast");
        brightness = mParameters.getInt("brightness");
        mred_eye = mParameters.getInt("red");
        flash = mParameters.getInt("flash");
        caf = mParameters.getInt("caf");
        rotation = mParameters.getInt("picture-rotation");

        FW3A_GetSettings();
        if(contrast != -1)
            fobj->settings_2a.general.contrast = contrast;

        if(brightness != -1) 
            fobj->settings_2a.general.brightness = brightness;

        if(saturation!= -1)
            fobj->settings_2a.general.saturation = saturation;
        
        if(sharpness != -1)
            fobj->settings_2a.general.sharpness = sharpness;
        
        if(scene!= -1)
            fobj->settings_2a.general.scene = (FW3A_SCENE_MODE) scene;
        
        if(effects!= -1)
            fobj->settings_2a.general.effects = (FW3A_CONFIG_EFFECTS) effects;

        if(wb!= -1)
            fobj->settings_2a.awb.mode = (WHITE_BALANCE_MODE_VALUES) wb;

        if(iso!= -1)
            fobj->settings_2a.ae.iso = (EXPOSURE_ISO_VALUES) iso;

        if(af!= -1)
            fobj->settings_2a.af.focus_mode = (FOCUS_MODE_VALUES) af;

        if(exposure!= -1)
            fobj->settings_2a.ae.mode = (EXPOSURE_MODE_VALUES) exposure;

        if(compensation!= -1)
            fobj->settings_2a.ae.compensation = compensation;

        if(mflash != flash){
            mflash = flash;
            fobj->settings_2a.general.flash_mode = (FW3A_FLASH_MODE) mflash;
        }

        fobj->settings_2a.af.focus_mode = FOCUS_MODE_AF_AUTO;
        fobj->settings_2a.af.spot_weighting = FOCUS_SPOT_MULTI_NORMAL;

        FW3A_SetSettings();

        LOGD("mcapture_mode = %d", mcapture_mode);
        
        if(mcaf != caf){
            mcaf = caf;
            Message msg;
            msg.command = mcaf ? PREVIEW_CAF_START : PREVIEW_CAF_STOP;
            previewThreadCommandQ.put(&msg);
            previewThreadAckQ.get(&msg);
            return msg.command == PREVIEW_ACK ? NO_ERROR : INVALID_OPERATION;
        }
        
    }

#endif
    
    LOG_FUNCTION_NAME_EXIT
    return NO_ERROR;
}

CameraParameters CameraHal::getParameters() const
{
    LOG_FUNCTION_NAME
    CameraParameters params;
    int iso, af, wb, exposure, scene, effects, compensation;
    int saturation, sharpness, contrast, brightness;

    {
        Mutex::Autolock lock(mLock);
        params = mParameters;
    }

    LOG_FUNCTION_NAME_EXIT
    return params;
}

status_t  CameraHal::dump(int fd, const Vector<String16>& args) const
{
    return 0;
}

void CameraHal::dumpFrame(void *buffer, int size, char *path)
{
    FILE* fIn = NULL;

    fIn = fopen(path, "w");
    if ( fIn == NULL ) {
        LOGE("\n\n\n\nError: failed to open the file %s for writing\n\n\n\n", path);
        return;
    }
    
    fwrite((void *)buffer, 1, size, fIn);
    fclose(fIn);

}

void CameraHal::release()
{
}

int CameraHal::onSnapshot(void *priv, void *buf, int width, int height)
{
    unsigned int snapshotMessage[5];

    CameraHal* camHal = reinterpret_cast<CameraHal*>(priv);

    LOG_FUNCTION_NAME

    snapshotMessage[0] = SNAPSHOT_THREAD_START;
    snapshotMessage[1] = (unsigned int) buf;
    snapshotMessage[2] = width;
    snapshotMessage[3] = height;
    snapshotMessage[4] = camHal->mZoomTarget;

    write(camHal->snapshotPipe[1], &snapshotMessage, sizeof(snapshotMessage));

    LOG_FUNCTION_NAME_EXIT

    return 0;
}

int CameraHal::onSaveH3A(void *priv, void *buf, int size)
{
    CameraHal* camHal = reinterpret_cast<CameraHal*>(priv);

    LOGD("Observer onSaveH3A\n");
    camHal->SaveFile(NULL, (char*)"h3a", buf, size);

    return 0;
}

int CameraHal::onSaveLSC(void *priv, void *buf, int size)
{
    CameraHal* camHal = reinterpret_cast<CameraHal*>(priv);

    LOGD("Observer onSaveLSC\n");
    camHal->SaveFile(NULL, (char*)"lsc", buf, size);

    return 0;
}

int CameraHal::onSaveRAW(void *priv, void *buf, int size)
{
    CameraHal* camHal = reinterpret_cast<CameraHal*>(priv);

    LOGD("Observer onSaveRAW\n");
    camHal->SaveFile(NULL, (char*)"raw", buf, size);

    return 0;
}


int CameraHal::SaveFile(char *filename, char *ext, void *buffer, int size)
{
    LOG_FUNCTION_NAME
    //Store image
    char fn [512];

    if (filename) {
      strcpy(fn,filename);
    } else {
      if (ext==NULL) ext = (char*)"tmp";
      sprintf(fn, PHOTO_PATH, file_index, ext);
    }
    file_index++;
    LOGD("Writing to file: %s", fn);
    int fd = open(fn, O_RDWR | O_CREAT | O_SYNC);
    if (fd < 0) {
        LOGE("Cannot open file %s : %s", fn, strerror(errno));
        return -1;
    } else {
    
        int written = 0;
        int page_block, page = 0;
        int cnt = 0;
        int nw;
        char *wr_buff = (char*)buffer;
        LOGD("Jpeg size %d", size);
        page_block = size / 20;
        while (written < size ) {
          nw = size - written;
          nw = (nw>512*1024)?8*1024:nw;
          
          nw = ::write(fd, wr_buff, nw);
          if (nw<0) {
              LOGD("write fail nw=%d, %s", nw, strerror(errno));
            break;
          }
          wr_buff += nw;
          written += nw;
          cnt++;
          
          page    += nw;
          if (page>=page_block){
              page = 0;
              LOGD("Percent %6.2f, wn=%5d, total=%8d, jpeg_size=%8d", 
                  ((float)written)/size, nw, written, size);
          }
        }

        close(fd);
     
        return 0;
    }
}


sp<IMemoryHeap> CameraHal::getPreviewHeap() const
{
    LOG_FUNCTION_NAME
    return 0;
}

sp<CameraHardwareInterface> CameraHal::createInstance()
{
    LOG_FUNCTION_NAME

    if (singleton != 0) {
        sp<CameraHardwareInterface> hardware = singleton.promote();
        if (hardware != 0) {
            return hardware;
        }
    }

    sp<CameraHardwareInterface> hardware(new CameraHal());

    singleton = hardware;
    return hardware;
} 

extern "C" sp<CameraHardwareInterface> openCameraHardware()
{
    LOGD("opening ti camera hal\n");
    return CameraHal::createInstance();
}


}; // namespace android
