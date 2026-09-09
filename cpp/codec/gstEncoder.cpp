/*
 * Copyright (c) 2018, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "gstEncoder.h"
#include "gstWebRTC.h"

#include "RTSPServer.h"
#include "WebRTCServer.h"

#include "filesystem.h"
#include "timespec.h"
#include "logging.h"

#include "cudaColorspace.h"

#define GST_USE_UNSTABLE_API
#include <gst/app/gstappsrc.h>
#include <gst/webrtc/webrtc.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <cstdio>

#include <sstream>

// TEMP DEBUG: unconditional stderr logging for smoke-testing (remove me).
// Bypasses the jetson-utils logger entirely so output can't be hidden by
// log levels or stdout redirection/buffering.
#define PROBE(fmt, ...) do { \
	fprintf(stderr, "!!! gstEncoder.cpp:%d " fmt "\n", __LINE__, ##__VA_ARGS__); \
	fflush(stderr); \
} while(0)

// supported video file extensions
const char* gstEncoder::SupportedExtensions[] = { "mkv", "mp4", "qt", 
										"flv", "avi", "h264", 
										"h265", NULL };

bool gstEncoder::IsSupportedExtension( const char* ext )
{
	if( !ext )
		return false;

	uint32_t extCount = 0;

	while(true)
	{
		if( !SupportedExtensions[extCount] )
			break;

		if( strcasecmp(SupportedExtensions[extCount], ext) == 0 )
			return true;

		extCount++;
	}

	return false;
}


// constructor
gstEncoder::gstEncoder( const videoOptions& options ) : videoOutput(options)
{
	// TEMP DEBUG: remove me
	PROBE("ctor: codec=%d layers=%zu input=%ux%u resource=%s",
		 (int)options.codec, options.layers.size(),
		 options.width, options.height, options.resource.string.c_str());

	mAppSrc       = NULL;
	mBus          = NULL;
	mBufferCaps   = NULL;
	mPipeline     = NULL;
	mRTSPServer   = NULL;
	mWebRTCServer = NULL;
	mNeedData     = false;

	mBufferYUV.SetThreaded(false);

	for( uint32_t n=0; n < YUVBufferCount; n++ )
	{
		mBufferBusy[n].store(false);
	}
}


// destructor	
gstEncoder::~gstEncoder()
{
	Close();

	if( mRTSPServer != NULL )
	{
		mRTSPServer->Release();
		mRTSPServer = NULL;
	}
	
	if( mWebRTCServer != NULL )
	{
		mWebRTCServer->Release();
		mWebRTCServer = NULL;
	}
	
	destroyPipeline();
}


// destroyPipeline
void gstEncoder::destroyPipeline()
{
	if( mAppSrc != NULL )
	{
		gst_object_unref(mAppSrc);
		mAppSrc = NULL;
	}

	if( mBus != NULL )
	{
		gst_object_unref(mBus);
		mBus = NULL;
	}

	if( mPipeline != NULL )
	{
		gst_element_set_state(mPipeline, GST_STATE_NULL);
		gst_object_unref(mPipeline);
		mPipeline = NULL;
	}
}


// Create
gstEncoder* gstEncoder::Create( const videoOptions& options )
{
	gstEncoder* enc = new gstEncoder(options);
	
	if( !enc )
		return NULL;
	
	if( !enc->init() )
	{
		PROBE("gstEncoder -- failed to create encoder engine");
		return NULL;
	}
	
	return enc;
}


// Create
gstEncoder* gstEncoder::Create( const URI& resource, videoOptions::Codec codec )
{
	videoOptions opt;

	opt.resource = resource;
	opt.codec    = codec;
	opt.ioType   = videoOptions::OUTPUT;

	return Create(opt);
}
	

// initPipeline
bool gstEncoder::initPipeline()
{
	// check for default codec
	if( mOptions.codec == videoOptions::CODEC_UNKNOWN )
	{
		PROBE("gstEncoder -- codec not specified, defaulting to H.264");
		mOptions.codec = videoOptions::CODEC_H264;
	}

	// check if default framerate is needed
	if( mOptions.frameRate < 0 )
		mOptions.frameRate = 30;

	// set default bitrate if needed
	if( mOptions.bitRate == 0 )
		mOptions.bitRate = 4000000; 
	
	// build pipeline string
	if( !buildLaunchStr() )
	{
		PROBE("gstEncoder -- failed to build pipeline string");
		return false;
	}
	
	// create the pipeline
	GError* err = NULL;
	mPipeline = gst_parse_launch(mLaunchStr.c_str(), &err);

	if( err != NULL )
	{
		PROBE("gstEncoder -- failed to create pipeline");
		PROBE("   (%s)", err->message);
		g_error_free(err);
		return false;
	}
	
	GstPipeline* pipeline = GST_PIPELINE(mPipeline);

	if( !pipeline )
	{
		PROBE("gstEncoder -- failed to cast GstElement into GstPipeline");
		return false;
	}	
	
	// retrieve pipeline bus
	mBus = gst_pipeline_get_bus(pipeline);

	if( !mBus )
	{
		PROBE("gstEncoder -- failed to retrieve GstBus from pipeline");
		return false;
	}
	
	// add watch for messages (disabled when we poll the bus ourselves, instead of gmainloop)
	//gst_bus_add_watch(mBus, (GstBusFunc)gst_message_print, NULL);

	// get the appsrc element
	GstElement* appsrcElement = gst_bin_get_by_name(GST_BIN(pipeline), "mysource");
	GstAppSrc* appsrc = GST_APP_SRC(appsrcElement);

	if( !appsrcElement || !appsrc )
	{
		PROBE("gstEncoder -- failed to retrieve appsrc element from pipeline");
		return false;
	}
	
	mAppSrc = appsrcElement;

	g_signal_connect(appsrcElement, "need-data", G_CALLBACK(onNeedData), this);
	g_signal_connect(appsrcElement, "enough-data", G_CALLBACK(onEnoughData), this);
	
	return true;
}


// init
bool gstEncoder::init()
{
	// initialize GStreamer libraries
	if( !gstreamerInit() )
	{
		PROBE("failed to initialize gstreamer API");
		return false;
	}

	// create GStreamer pipeline
	if( !initPipeline() )
	{
		PROBE("failed to create encoder pipeline");
		return false;
	}

	// create servers for RTSP/WebRTC streams
	if( mOptions.resource.protocol == "rtsp" )
	{
		mRTSPServer = RTSPServer::Create(mOptions.resource.port);
		
		if( !mRTSPServer )
			return false;
		
		mRTSPServer->AddRoute(mOptions.resource.path.c_str(), mPipeline);
	}
	else if( mOptions.resource.protocol == "webrtc" )
	{
		mWebRTCServer = WebRTCServer::Create(mOptions.resource.port, mOptions.stunServer.c_str(),
									  mOptions.sslCert.c_str(), mOptions.sslKey.c_str());
		
		if( !mWebRTCServer )
			return false;
		
		mWebRTCServer->AddRoute(mOptions.resource.path.c_str(), onWebsocketMessage, this, WEBRTC_VIDEO|WEBRTC_SEND|WEBRTC_PUBLIC|WEBRTC_MULTI_CLIENT);
	}		

	return true;
}
	

// buildCapsStr
bool gstEncoder::buildCapsStr()
{
	std::ostringstream ss;

#if GST_CHECK_VERSION(1,0,0)
	ss << "video/x-raw";
	ss << ", width=" << GetWidth();
	ss << ", height=" << GetHeight();
	ss << ", format=(string)I420";
	ss << ", framerate=" << (int)mOptions.frameRate << "/1";
#else
	ss << "video/x-raw-yuv";
	ss << ",width=" << GetWidth();
	ss << ",height=" << GetHeight();
	ss << ",format=(fourcc)I420";
	ss << ",framerate=" << (int)mOptions.frameRate << "/1";
#endif
	
	mCapsStr = ss.str();
	PROBE("gstEncoder -- new caps: %s", mCapsStr.c_str());
	return true;
}
	
	

// gst_encoder_append_settings
// appends the encoder element (with the given element name), its tuning properties,
// and its output caps to the launch string.  factored out of buildLaunchStr() so the
// simulcast path can emit one encoder per layer with a per-layer bitrate.
static void gst_encoder_append_settings( std::ostringstream& ss, videoOptions& options, const char* encoder, uint32_t bitRate, const std::string& name )
{
	ss << encoder << " name=" << name << " ";

	if( strncmp(encoder, "nvh264enc", 9) == 0 )
	{
		// desktop NVENC (nvcodec): bitrate is in kbit/sec (like x264enc). Only set bitrate and rely
		// on element defaults for everything else, so the pipeline launches regardless of nvcodec
		// plugin version (preset/rc-mode/zerolatency property names vary across versions).
		ss << "bitrate=" << bitRate / 1000 << " ";
	}
	else if( options.codecType == videoOptions::CODEC_CPU )
	{
		if( options.codec == videoOptions::CODEC_H264 || options.codec == videoOptions::CODEC_H265 )
		{
			ss << "bitrate=" << bitRate / 1000 << " ";	// x264enc/x265enc bitrates are in kbits
			ss << "speed-preset=ultrafast tune=zerolatency ";
		}
		else if( options.codec == videoOptions::CODEC_VP8 || options.codec == videoOptions::CODEC_VP9 )
		{
			ss << "target-bitrate=" << bitRate << " ";

			if( options.deviceType == videoOptions::DEVICE_IP )
				ss << "keyframe-max-dist=30 ";
		}
	}
	else if( options.codec != videoOptions::CODEC_MJPEG )
	{
		ss << "bitrate=" << bitRate << " ";

		if( options.deviceType == videoOptions::DEVICE_IP )
		{
			if( options.codecType == videoOptions::CODEC_V4L2 )
                ss << "insert-sps-pps=1 insert-vui=1 idrinterval=" << options.maxIFrameInterval
                   << " peak-bitrate=30000000 control-rate=1 vbv-size=450000 ";
			else if( options.codecType == videoOptions::CODEC_OMX )
				ss << "insert-sps-pps=1 insert-vui=1 ";
		}

		if( options.codecType == videoOptions::CODEC_V4L2 )
			ss << "maxperf-enable=1 ";
	}

	if( options.codec == videoOptions::CODEC_H264 || options.codec == videoOptions::CODEC_H265 )
	{
		// send keyframes/I-frames more frequently for network streams
		if( options.deviceType == videoOptions::DEVICE_IP ) {
			if (options.maxIFrameInterval == 0){
				options.maxIFrameInterval = 30;
			}
			#ifdef __aarch64__
			ss << "iframeinterval=" << std::to_string(options.maxIFrameInterval) << " insert-vui=1 ";
			#else
			if( strncmp(encoder, "nvh264enc", 9) == 0 )
				ss << "bframes=0 gop-size=" << std::to_string(options.maxIFrameInterval) << " ";  // nvcodec: gop-size, no key-int-max/insert-vui
			else
				ss << "bframes=0 key-int-max=" << std::to_string(options.maxIFrameInterval) << " insert-vui=1 ";
			#endif
		}

	}

	if( options.codec == videoOptions::CODEC_H264 ) {
	    ss << "! video/x-h264,profile=constrained-baseline ! ";
#ifndef __aarch64__
	    ss<<" h264parse config-interval=1 ! ";
#endif
	}
	else if( options.codec == videoOptions::CODEC_H265 )
		ss << "! video/x-h265 ! ";
	else if( options.codec == videoOptions::CODEC_VP8 )
		ss << "! video/x-vp8 ! ";
	else if( options.codec == videoOptions::CODEC_VP9 )
		ss << "! video/x-vp9 ! ";
	else if( options.codec == videoOptions::CODEC_MJPEG )
		ss << "! image/jpeg ! ";
}


// gst_encoder_append_udpsink
static void gst_encoder_append_udpsink( std::ostringstream& ss, const URI& uri )
{
	ss << "udpsink host=" << uri.location << " ";

	if( uri.port != 0 )
		ss << "port=" << uri.port;

	ss << " buffer-size=2000000";
	ss << " auto-multicast=true";
}


// gst_encoder_validate_simulcast
static bool gst_encoder_validate_simulcast( const videoOptions& options, const URI& uri )
{
	const size_t numLayers = options.layers.size();

	if( uri.protocol != "rtp" )
	{
		PROBE("gstEncoder -- simulcast output requires the rtp:// protocol (got '%s')", uri.protocol.c_str());
		return false;
	}

	for( size_t i=0; i < numLayers; i++ )
	{
		const videoOptions::SimulcastLayer& layer = options.layers[i];

		if( (layer.width == 0) != (layer.height == 0) )
		{
			PROBE("gstEncoder -- simulcast layer %zu: width and height must both be set, or both be 0 for full input resolution", i);
			return false;
		}

		if( (layer.width % 2) != 0 || (layer.height % 2) != 0 )
		{
			PROBE("gstEncoder -- simulcast layer %zu: width and height must be even (got %ux%u)", i, layer.width, layer.height);
			return false;
		}

		if( (options.width != 0 && layer.width > options.width) || (options.height != 0 && layer.height > options.height) )
		{
			PROBE("gstEncoder -- simulcast layer %zu: resolution %ux%u exceeds the input resolution %ux%u", i, layer.width, layer.height, options.width, options.height);
			return false;
		}

		if( layer.ssrc == 0 && options.ssrc == 0 )
		{
			PROBE("gstEncoder -- simulcast layer %zu: no SSRC and no base SSRC to derive one from (set videoOptions::ssrc or a per-layer ssrc)", i);
			return false;
		}
	}

	// effective SSRCs (explicit, or auto-derived as base SSRC + layer index) must be unique
	for( size_t i=0; i < numLayers; i++ )
	{
		const uint32_t ssrc_i = options.layers[i].ssrc != 0 ? options.layers[i].ssrc : options.ssrc + (uint32_t)i;

		for( size_t j=i+1; j < numLayers; j++ )
		{
			const uint32_t ssrc_j = options.layers[j].ssrc != 0 ? options.layers[j].ssrc : options.ssrc + (uint32_t)j;

			if( ssrc_i == ssrc_j )
			{
				PROBE("gstEncoder -- simulcast layers %zu and %zu have the same effective SSRC (%u)", i, j, ssrc_i);
				return false;
			}
		}
	}

	if( options.rescale )
		PROBE("gstEncoder -- videoOptions::rescale is ignored in simulcast mode (use per-layer width/height instead)");

	if( options.save.path.length() > 0 )
		PROBE("gstEncoder -- videoOptions::save is ignored in simulcast mode");

	return true;
}


// gst_encoder_build_simulcast
// builds the tail of the launch string for a multi-layer H.264 simulcast pipeline:
//
//   <input> ! <upload to GPU memory> ! tee name=simulcasttee
//     simulcasttee. ! queue ! <scale to layer 0> ! <encoder> ! rtph264pay ssrc=S0 ! f.
//     simulcasttee. ! queue ! <scale to layer 1> ! <encoder> ! rtph264pay ssrc=S1 ! f.
//     ...
//   funnel name=f ! udpsink ...
//
// all layers are interleaved onto the single UDP destination, and receivers tell
// them apart by SSRC.  note that plain udpsink carries no RTCP, so receivers cannot
// request keyframes (PLI/FIR) -- stream recovery relies entirely on the periodic
// IDR interval (videoOptions::maxIFrameInterval).
static bool gst_encoder_build_simulcast( std::ostringstream& ss, videoOptions& options, const URI& uri, const char* encoder )
{
	if( !gst_encoder_validate_simulcast(options, uri) )
		return false;

#ifdef __aarch64__
	if( options.codecType != videoOptions::CODEC_V4L2 )
	{
		PROBE("gstEncoder -- simulcast on Jetson requires the V4L2 hardware encoder (%s selected)", videoOptions::CodecTypeToStr(options.codecType));
		return false;
	}

	// upload to NVMM once, then each branch downscales on the VIC with its own nvvidconv
	ss << "nvvidconv name=vidconv ! video/x-raw(memory:NVMM) ! tee name=simulcasttee ";
#else
	// on x86, simulcast always uses NVENC (nvh264enc) regardless of GUNCAM_NVENC --
	// several parallel x264enc instances are too CPU-heavy, and cudascale's
	// CUDA-memory output feeds nvh264enc directly.
	if( strncmp(encoder, "nvh264enc", 9) != 0 )
	{
		PROBE("gstEncoder -- simulcast on x86 requires NVENC, overriding encoder '%s' with nvh264enc", encoder);
		encoder = "nvh264enc";
	}

	// upload to CUDA memory once, then each branch downscales on the GPU with cudascale
	ss << "cudaupload ! video/x-raw(memory:CUDAMemory) ! tee name=simulcasttee ";
#endif

	// normalize the IDR interval up front so every layer gets the same GOP settings
	// (gst_encoder_append_settings otherwise defaults it to 30 partway through the build)
	if( options.deviceType == videoOptions::DEVICE_IP && options.maxIFrameInterval == 0 )
		options.maxIFrameInterval = 30;

	const size_t numLayers = options.layers.size();

	for( size_t i=0; i < numLayers; i++ )
	{
		const videoOptions::SimulcastLayer& layer = options.layers[i];

		const uint32_t bitRate = layer.bitRate != 0 ? layer.bitRate : options.bitRate;
		const uint32_t ssrc    = layer.ssrc != 0 ? layer.ssrc : options.ssrc + (uint32_t)i;	// auto-derive from the base SSRC

		ss << "simulcasttee. ! queue ! ";

	#ifdef __aarch64__
		ss << "nvvidconv ! video/x-raw(memory:NVMM)";
	#else
		ss << "cudascale ! video/x-raw(memory:CUDAMemory)";
	#endif

		if( layer.width != 0 && layer.height != 0 )
			ss << ",width=" << layer.width << ",height=" << layer.height;

		ss << " ! ";

		gst_encoder_append_settings(ss, options, encoder, bitRate, "encoder" + std::to_string(i));

		// payload type: per-layer override, else videoOptions::payload_type, else the RTP default 96
		const uint32_t pt = layer.payloadType != 0 ? layer.payloadType
		                  : (options.payload_type != 0 ? options.payload_type : 96);

		ss << "rtph264pay name=pay_l" << i << " config-interval=1 aggregate-mode=1 mtu=" << options.mtu;
		ss << " pt=" << pt << " ssrc=" << ssrc << " ! f. ";
	}

	ss << "funnel name=f ! ";
	gst_encoder_append_udpsink(ss, uri);
	ss << " sync=false";

	return true;
}


// buildLaunchStr
bool gstEncoder::buildLaunchStr()
{
	// TEMP DEBUG: remove me
	PROBE("buildLaunchStr: layers=%zu codec=%d codecType=%d %ux%u",
		 mOptions.layers.size(), (int)mOptions.codec, (int)mOptions.codecType,
		 mOptions.width, mOptions.height);

	std::ostringstream ss;
	ss << "appsrc name=mysource is-live=true do-timestamp=true format=3";  // setup appsrc input element

	if( !mOptions.block){
		ss << " block=false";
	}

	if( mOptions.input_is_rgb ) {
		// This specifies that the input is RGB of the width and height
		ss << " caps=\"video/x-raw,format=RGB";
		ss << ",width=";
		ss << std::to_string(mOptions.width);
		ss << ",height=";
		ss << std::to_string(mOptions.height);
		ss << ",framerate="<<std::to_string(mOptions.frameRate)<<"/1\"";
	}

	if (mOptions.latestOnly) {
		ss << " ! queue max-size-buffers=3 leaky=downstream";
	}

	ss << " ! ";

	if ( mOptions.codec != videoOptions::CODEC_H264 && mOptions.rescale && mOptions.output_width != 0 and mOptions.output_height != 0 ) {
		ss << "videoconvert ! videoscale ! ";
		ss << "video/x-raw,";
		ss << "width=";
		ss << std::to_string(mOptions.output_width);
		ss << ",height=";
		ss << std::to_string(mOptions.output_height);
		ss << " ! ";
	}

	if ( mOptions.input_is_rgb ) {
		// convert to the format needed by the encoder
		ss << "videoconvert ! ";
		ss << "video/x-raw,format=I420 ! ";
	}
	
	const URI& uri = GetResource();

	// select the encoder
	const char* encoder = gst_select_encoder(mOptions.codec, mOptions.codecType);
	
	if( !encoder )
	{
		PROBE("gstEncoder -- unsupported codec requested (%s)", videoOptions::CodecToStr(mOptions.codec));
		PROBE("              supported encoder codecs are:");
		PROBE("                 * h264");
		PROBE("                 * h265");
		PROBE("                 * vp8");
		PROBE("                 * vp9");
		PROBE("                 * mjpeg");

		return false;
	}

	// simulcast (see videoOptions::layers): 2+ layers build a multi-encoder pipeline,
	// exactly 1 layer just overrides the equivalent top-level options below
	if( mOptions.layers.size() > 0 && mOptions.codec != videoOptions::CODEC_H264 )
	{
		PROBE("gstEncoder -- simulcast layers are only supported with the H264 codec (%s requested)", videoOptions::CodecToStr(mOptions.codec));
		return false;
	}

	if( mOptions.layers.size() > 1 )
	{
		if( !gst_encoder_build_simulcast(ss, mOptions, uri, encoder) )
			return false;

		mLaunchStr = ss.str();

		PROBE("gstEncoder -- pipeline launch string:");
		PROBE("%s", mLaunchStr.c_str());

		return true;
	}

	// effective encoder settings -- a single simulcast layer overrides these
	uint32_t encBitRate    = mOptions.bitRate;
	uint32_t rtpSSRC       = mOptions.ssrc;
	uint32_t rtpPT         = mOptions.payload_type;
	bool     rescale       = mOptions.rescale && mOptions.output_width != 0 && mOptions.output_height != 0;
	uint32_t rescaleWidth  = mOptions.output_width;
	uint32_t rescaleHeight = mOptions.output_height;

	if( mOptions.layers.size() == 1 )
	{
		const videoOptions::SimulcastLayer& layer = mOptions.layers[0];

		if( layer.bitRate != 0 )
			encBitRate = layer.bitRate;

		if( layer.ssrc != 0 )
			rtpSSRC = layer.ssrc;	// otherwise auto-derive: base SSRC + layer index (== base SSRC for layer 0)

		if( layer.payloadType != 0 )
			rtpPT = layer.payloadType;	// otherwise inherit videoOptions::payload_type

		if( layer.width != 0 && layer.height != 0 )
		{
			// H264 rescale is only implemented for the V4L2 encoder (the NVMM caps below)
			if( mOptions.codecType != videoOptions::CODEC_V4L2 )
				PROBE("gstEncoder -- H264 rescale requires the V4L2 encoder, the layer resolution %ux%u will be ignored", layer.width, layer.height);

			rescale       = true;
			rescaleWidth  = layer.width;
			rescaleHeight = layer.height;
		}
	}

	// the V4L2 encoders expect NVMM memory, so use nvvidconv to convert it
	if( mOptions.codecType == videoOptions::CODEC_V4L2 && mOptions.codec != videoOptions::CODEC_MJPEG ){
		ss << "nvvidconv name=vidconv ! video/x-raw(memory:NVMM)";
		if (mOptions.codec == videoOptions::CODEC_H264 && rescale){
			ss << ",width=";
			ss << std::to_string(rescaleWidth);
			ss << ",height=";
			ss << std::to_string(rescaleHeight);
		}

		ss << " ! ";
	}

	// setup the encoder and options
	gst_encoder_append_settings(ss, mOptions, encoder, encBitRate, "encoder");

	if( mOptions.save.path.length() > 0 )
	{
		ss << "tee name=savetee savetee. ! queue ! ";
		
		if( !gst_build_filesink(mOptions.save, mOptions.codec, ss) )
			return false;

		ss << "savetee. ! queue ! ";
	}
	
	if( uri.protocol == "file" )
	{
		if( !gst_build_filesink(uri, mOptions.codec, ss) )
			return false;
	}
	else if( uri.protocol == "rtp" || uri.protocol == "rtsp" || uri.protocol == "webrtc" )
	{
		if( mOptions.codec == videoOptions::CODEC_H264 )
			ss << "rtph264pay";
		else if( mOptions.codec == videoOptions::CODEC_H265 )
			ss << "rtph265pay";
		else if( mOptions.codec == videoOptions::CODEC_VP8 )
			ss << "rtpvp8pay picture-id-mode=15-bit";
		else if( mOptions.codec == videoOptions::CODEC_VP9 )
			ss << "rtpvp9pay";
		else if( mOptions.codec == videoOptions::CODEC_MJPEG )
			ss << "rtpjpegpay";

		if( mOptions.codec == videoOptions::CODEC_H264 || mOptions.codec == videoOptions::CODEC_H265 ) 
			ss << " config-interval=1 aggregate-mode=1 mtu="<<mOptions.mtu;

		if (rtpPT != 0){
			ss << " pt=";
			ss << std::to_string(rtpPT);
		}

		if (rtpSSRC != 0){
			ss << " ssrc=";
			ss << std::to_string(rtpSSRC);
		}

		if( uri.protocol == "rtsp" )
			ss << " name=pay0";	 // GstRTSPServer expects the payloaders to be named pay0, pay1, ect
		else
			ss << " ! ";
		
		if( uri.protocol == "rtp" )
		{
			gst_encoder_append_udpsink(ss, uri);
		}
		else if( uri.protocol == "webrtc" )
		{
			ss << "application/x-rtp,media=video,encoding-name=" << videoOptions::CodecToStr(mOptions.codec) << ",clock-rate=90000,payload=96 ! ";
			ss << "tee name=videotee ! queue ! fakesink";  // webrtcbin's will be added when clients connect
		}
	}
	else if( uri.protocol == "rtpmp2ts" )
	{
		// https://forums.developer.nvidia.com/t/gstreamer-udp-to-vlc/215349/5
		if( mOptions.codec == videoOptions::CODEC_H264 ) 
			ss << "h264parse config-interval=1 ! mpegtsmux ! rtpmp2tpay ! udpsink host=";
		else if (mOptions.codec == videoOptions::CODEC_H265 )
			ss << "h265parse config-interval=1 ! mpegtsmux ! rtpmp2tpay ! udpsink host=";
		else
		{
			PROBE("gstEncoder -- rtpmp2ts output only supports h264 and h265. Unsupported codec (%s)", uri.extension.c_str());
			return false;
		}
 		
		ss << uri.location << " ";

		if( uri.port != 0 )
			ss << "port=" << uri.port;

		ss << " auto-multicast=true";
	}
	else if( uri.protocol == "rtmp" )
	{
		ss << "flvmux streamable=true ! queue ! rtmpsink location=";
		ss << uri.string << " ";
	}
	else
	{
		PROBE("gstEncoder -- invalid protocol (%s)", uri.protocol.c_str());
		return false;
	}

	ss << " sync=false";

	mLaunchStr = ss.str();

	PROBE("gstEncoder -- pipeline launch string:");
	PROBE("%s", mLaunchStr.c_str());

	return true;
}


// onNeedData
void gstEncoder::onNeedData( GstElement* pipeline, guint size, gpointer user_data )
{
	//LogDebug(LOG_GSTREAMER "gstEncoder -- appsrc requesting data (%u bytes)\n", size);
	
	if( !user_data )
		return;

	gstEncoder* enc = (gstEncoder*)user_data;
	enc->mNeedData  = true;
}
 

// onEnoughData
void gstEncoder::onEnoughData( GstElement* pipeline, gpointer user_data )
{
	PROBE("gstEncoder -- appsrc signalling enough data");

	if( !user_data )
		return;

	gstEncoder* enc = (gstEncoder*)user_data;
	enc->mNeedData  = false;
}


// context handed to a wrapped GstBuffer's release callback
struct gstYUVBufferRef
{
	gstEncoder* encoder;
	int         slot;
};


// onBufferReleased
void gstEncoder::onBufferReleased( void* user_data )
{
	gstYUVBufferRef* ref = (gstYUVBufferRef*)user_data;

	if( !ref )
		return;

	if( ref->encoder != NULL && ref->slot >= 0 && ref->slot < (int)YUVBufferCount )
		ref->encoder->mBufferBusy[ref->slot].store(false);

	delete ref;
}


// encodeYUV
bool gstEncoder::encodeYUV( void* buffer, size_t size, int slot )
{
	if( !buffer || size == 0 )
		return false;
	
	// confirm the stream is open
	if( !mStreaming )
	{
		if( !Open() )
			return false;
	}

	// check to see if data can be accepted
	// 20240307 - disabling this because with WebRTC sometimes it gets stuck in 'pipeline full' state
	/*if( !mNeedData )
	{
		if( mOptions.frameCount % 25 == 0 )
			PROBE("gstEncoder -- pipeline full, skipping frame %zu (%ux%u, %zu bytes)", mOptions.frameCount, mOptions.width, mOptions.height, size);
		
		return true;
	}*/

	// construct the buffer caps for this size image
	if( !mBufferCaps )
	{
		if( !buildCapsStr() )
		{
			PROBE("gstEncoder -- failed to build caps string");
			return false;
		}

		mBufferCaps = gst_caps_from_string(mCapsStr.c_str());

		if( !mBufferCaps )
		{
			PROBE("gstEncoder -- failed to parse caps from string:");
			PROBE("   %s", mCapsStr.c_str());
			return false;
		}

	#if GST_CHECK_VERSION(1,0,0)
		gst_app_src_set_caps(GST_APP_SRC(mAppSrc), mBufferCaps);
	#endif
	}

#if GST_CHECK_VERSION(1,0,0)
	// Wrap the ring-buffer slot directly into a GstBuffer instead of allocating a new
	// buffer and memcpy'ing into it. The slot's memory is CUDA-accessible (ZeroCopy) and
	// already holds this frame's I420 data (converted into it on Tegra, D2H-copied into it
	// on discrete GPUs). The slot's busy flag is held until GStreamer frees the buffer
	// (onBufferReleased), so Render() won't overwrite a slot still in flight downstream.
	if( slot >= 0 && slot < (int)YUVBufferCount )
		mBufferBusy[slot].store(true);

	gstYUVBufferRef* bufferRef = new gstYUVBufferRef{ this, slot };

	GstBuffer* gstBuffer = gst_buffer_new_wrapped_full(GST_MEMORY_FLAG_READONLY, buffer, size, 0, size, bufferRef, onBufferReleased);

	if( !gstBuffer )
	{
		PROBE("gstEncoder -- failed to wrap gstreamer buffer memory (%zu bytes)", size);
		onBufferReleased(bufferRef);	// clears the busy flag and frees the ref
		return false;
	}
#else
	(void)slot;	// slot lifetime tracking only applies to the 1.0 zero-copy wrap path

	// convert memory to GstBuffer
	GstBuffer* gstBuffer = gst_buffer_new();

	GST_BUFFER_MALLOCDATA(gstBuffer) = (guint8*)g_malloc(size);
	GST_BUFFER_DATA(gstBuffer) = GST_BUFFER_MALLOCDATA(gstBuffer);
	GST_BUFFER_SIZE(gstBuffer) = size;
	
	//static size_t num_frame = 0;
	//GST_BUFFER_TIMESTAMP(gstBuffer) = (GstClockTime)((num_frame / 30.0) * 1e9);	// for 1.0, use GST_BUFFER_PTS or GST_BUFFER_DTS instead
	//num_frame++;

	if( mBufferCaps != NULL )
		gst_buffer_set_caps(gstBuffer, mBufferCaps);
	
	memcpy(GST_BUFFER_DATA(gstBuffer), buffer, size);
#endif

	// queue buffer to gstreamer
	while( true )
	{
		GstFlowReturn ret;	
		g_signal_emit_by_name(mAppSrc, "push-buffer", gstBuffer, &ret);
		
		if( ret >= 0 )
		{
			gst_buffer_unref(gstBuffer);
			break;
		}
		
		PROBE("gstEncoder -- an error occurred pushing appsrc buffer (result=%i '%s')", (int)ret, gst_flow_get_name(ret));
		
		// check to make sure the pipeline is still playing (some pipelines like RTSP server may disconnect)
		GstState state = GST_STATE_VOID_PENDING;
		gst_element_get_state(mPipeline, &state, NULL, GST_CLOCK_TIME_NONE);
	
		if( state != GST_STATE_PLAYING )
		{
			PROBE("gstEncoder -- pipeline is in the '%s' state, restarting pipeline...", gst_element_state_get_name(state));
			
			mStreaming = false;
			
			if( !Open() )
			{
				gst_buffer_unref(gstBuffer);
				return false;
			}
		}
	}
	
	checkMsgBus();
	return true;
}


// Render
bool gstEncoder::Render( void* image, uint32_t width, uint32_t height, imageFormat format, cudaStream_t stream )
{	
	// update the webrtc server if needed
	if( mWebRTCServer != NULL && !mWebRTCServer->IsThreaded() )
		mWebRTCServer->ProcessRequests();	
	
	// increment frame counter
	mOptions.frameCount += 1;
		
	// verify image dimensions
	if( !image || width == 0 || height == 0 )
		return false;

	if( mOptions.width != width || mOptions.height != height )
	{
		if( mOptions.width != 0 || mOptions.height != 0 )
			PROBE("gstEncoder -- resolution changing from (%ux%u) to (%ux%u)", mOptions.width, mOptions.height, width, height);
		
		mOptions.width  = width;
		mOptions.height = height;

		if( mBufferCaps != NULL )
		{
			gst_object_unref(mBufferCaps);
			mBufferCaps = NULL;
			
			destroyPipeline();
		
			mStreaming = false;
			
			if( !initPipeline() || !Open() )
			{
				PROBE("failed to re-initialize encoder with new dimensions (%ux%u)", width, height);
				return false;
			}
		}
		
		/*// nvbufsurface: NvBufSurfaceCopy: buffer param mismatch
		GstElement* vidconv = gst_bin_get_by_name(GST_BIN(mPipeline), "vidconv");
		GstElement* encoder = gst_bin_get_by_name(GST_BIN(mPipeline), "encoder");
		
		if( vidconv != NULL && encoder != NULL )
		{
			gst_element_set_state(mAppSrc, GST_STATE_NULL);
			gst_element_set_state(vidconv, GST_STATE_NULL);
			gst_element_set_state(encoder, GST_STATE_NULL);
			gst_element_set_state(mAppSrc, GST_STATE_PLAYING);
			gst_element_set_state(vidconv, GST_STATE_PLAYING);
			gst_element_set_state(encoder, GST_STATE_PLAYING);
			gst_object_unref(vidconv);
			gst_object_unref(encoder);
			usleep(500*1000);
			checkMsgBus();
		}*/
	}

	// error checking / return
	bool enc_success = false;

	#define render_end()	\
		const bool substreams_success = videoOutput::Render(image, width, height, format); \
		return enc_success & substreams_success;

	// allocate color conversion buffer
	const size_t i420Size = imageFormatSize(IMAGE_I420, width, height);
	// nextYUV must be host-mapped (ZeroCopy): on Tegra the convert kernel writes I420 straight
	// into it, on discrete GPUs the I420 image is bulk-copied D->H into it. It is then wrapped
	// zero-copy into a GstBuffer (no host memcpy). RingBuffer::Threaded (no ZeroCopy) allocates
	// device-only memory (cudaMalloc), which is not CPU/GStreamer-accessible.
	if( !mBufferYUV.Alloc(YUVBufferCount, i420Size, RingBuffer::ZeroCopy) )
	{
		PROBE("gstEncoder -- failed to allocate buffers (%zu bytes each)", i420Size);
		enc_success = false;
		render_end();
	}

	// perform colorspace conversion
	void* nextYUV = mBufferYUV.Next(RingBuffer::Write);
	const int yuvSlot = (int)mBufferYUV.GetLatestWrite();

	// don't reuse a slot that GStreamer is still holding downstream (wrapped zero-copy).
	// With YUVBufferCount slots >> pipeline depth this is essentially never hit, but if it
	// is, skip this frame rather than corrupt an in-flight buffer.
	if( mBufferBusy[yuvSlot].load() )
	{		
		PROBE("gstEncoder -- all YUV push buffers in flight, skipping frame %zu", mOptions.frameCount);
		enc_success = true;
		render_end();
	}

#if defined(__aarch64__)
	// Tegra: nextYUV is unified memory, so convert straight into it.
	const bool convert_ok = !CUDA_FAILED(cudaConvertColor(image, format, nextYUV, IMAGE_I420, width, height, stream));
#else
	// Discrete GPU (x86): nextYUV is ZeroCopy (host-mapped) memory. Converting RGB->I420 directly
	// into it scatters per-pixel writes across PCIe (~12ms/frame - this dominated the GPU). Instead
	// convert into a device buffer (coalesced device writes, sub-ms) then do a single bulk D2H copy
	// into the host push buffer. thread_local: each stream's Render runs on its own dedicated thread,
	// so the per-thread device buffer avoids any cross-encoder race.
    bool convert_ok = false;
    {
        thread_local TL_si420 s_dev_i420;
        thread_local size_t s_dev_size = 0;
        if (s_dev_size < i420Size) {
            if (s_dev_i420._ptr) {
                cudaFree(s_dev_i420._ptr);
            }
            if (CUDA_FAILED(cudaMalloc(&s_dev_i420._ptr, i420Size))) {
                s_dev_i420._ptr = nullptr;
                s_dev_size = 0;
            } else {
                s_dev_size = i420Size;
            }
        }
        if (s_dev_i420._ptr && !CUDA_FAILED(cudaConvertColor(image, format, s_dev_i420._ptr, IMAGE_I420, width, height, stream))) {
            convert_ok = !CUDA_FAILED(cudaMemcpyAsync(nextYUV, s_dev_i420._ptr, i420Size, cudaMemcpyDeviceToHost, stream));
        }
	}
#endif

	if( !convert_ok ) 
	{
		PROBE("gstEncoder::Render() -- unsupported image format (%s)", imageFormatToStr(format));
		PROBE("                        supported formats are:");
		PROBE("                            * rgb8");
		PROBE("                            * rgba8");
		PROBE("                            * rgb32f");
		PROBE("                            * rgba32f");

		enc_success = false;
		render_end();
	}

    if( stream != 0 ) 
    {
        CUDA(cudaStreamSynchronize(stream));
    } 
    else 
    {
        CUDA(cudaDeviceSynchronize());
    }
	// encode YUV buffer (wrapped zero-copy from ring slot yuvSlot)
	enc_success = encodeYUV(nextYUV, i420Size, yuvSlot);

	// render sub-streams
	render_end();	
}


// Open
bool gstEncoder::Open()
{
	if( mStreaming )
		return true;

	// transition pipline to STATE_PLAYING
	PROBE("gstEncoder -- starting pipeline, transitioning to GST_STATE_PLAYING");

	const GstStateChangeReturn result = gst_element_set_state(mPipeline, GST_STATE_PLAYING);

	if( result == GST_STATE_CHANGE_ASYNC )
	{
		PROBE("gstEncoder -- queued state to GST_STATE_PLAYING => GST_STATE_CHANGE_ASYNC");
		
#if 0
		GstMessage* asyncMsg = gst_bus_timed_pop_filtered(mBus, 5 * GST_SECOND, 
    	 					      (GstMessageType)(GST_MESSAGE_ASYNC_DONE|GST_MESSAGE_ERROR)); 

		if( asyncMsg != NULL )
		{
			gst_message_print(mBus, asyncMsg, this);
			gst_message_unref(asyncMsg);
		}
		else
			PROBE("gstEncoder -- NULL message after transitioning pipeline to PLAYING...");
#endif
	}
	else if( result != GST_STATE_CHANGE_SUCCESS )
	{
		PROBE("gstEncoder -- failed to set pipeline state to PLAYING (error %u)", result);
		return false;
	}

	checkMsgBus();
	usleep(100 * 1000);
	checkMsgBus();

	mStreaming = true;
	return true;
}
	

// Close
void gstEncoder::Close()
{
	if( !mStreaming )
		return;

	// send EOS
	mNeedData = false;
	
	PROBE("gstEncoder -- shutting down pipeline, sending EOS");
	GstFlowReturn eos_result = gst_app_src_end_of_stream(GST_APP_SRC(mAppSrc));

	if( eos_result != 0 )
		PROBE("gstEncoder -- failed sending appsrc EOS (result %u)", eos_result);

	sleep(1);

	// stop pipeline
	PROBE("gstEncoder -- transitioning pipeline to GST_STATE_NULL");

	const GstStateChangeReturn result = gst_element_set_state(mPipeline, GST_STATE_NULL);

	if( result != GST_STATE_CHANGE_SUCCESS )
		PROBE("gstEncoder -- failed to set pipeline state to NULL (error %u)", result);

	sleep(1);
	checkMsgBus();	
	mStreaming = false;
	PROBE("gstEncoder -- pipeline stopped");
}


// checkMsgBus
void gstEncoder::checkMsgBus()
{
	while(true)
	{
		GstMessage* msg = gst_bus_pop(mBus);

		if( !msg )
			break;

		gst_message_print(mBus, msg, this);
		gst_message_unref(msg);
	}
}


// onWebsocketMessage
void gstEncoder::onWebsocketMessage( WebRTCPeer* peer, const char* message, size_t message_size, void* user_data )
{
	if( !user_data )
		return;
	
	gstEncoder* encoder = (gstEncoder*)user_data;
	gstWebRTC::PeerContext* peer_context = (gstWebRTC::PeerContext*)peer->user_data;
	
	if( peer->flags & WEBRTC_PEER_CONNECTING )
	{
		LogVerbose(LOG_WEBRTC "new WebRTC peer connecting (%s, peer_id=%u)\n", peer->ip_address.c_str(), peer->ID);
		
		// new peer context
		peer_context = new gstWebRTC::PeerContext();
		peer->user_data = peer_context;
		
		// create a new queue element
		gchar* tmp = g_strdup_printf("queue-%u", peer->ID);
		peer_context->queue = gst_element_factory_make("queue", tmp);
		g_assert_nonnull(peer_context->queue);
		gst_object_ref(peer_context->queue);
		g_free(tmp);
		
		// create a new webrtcbin element
		tmp = g_strdup_printf("webrtcbin-%u", peer->ID);
		peer_context->webrtcbin = gst_element_factory_make("webrtcbin", tmp);
		g_assert_nonnull(peer_context->webrtcbin);
		gst_object_ref(peer_context->webrtcbin);
		g_free(tmp);
		
		// set webrtcbin properties
		const char* stun_server = peer->server->GetSTUNServer();
		
		if( stun_server != NULL && strlen(stun_server) > 0 )
		{
		    std::string stun_url = std::string("stun://") + stun_server;
		    g_object_set(peer_context->webrtcbin, "stun-server", stun_url.c_str(), NULL);
		}
		
		g_object_set(peer_context->webrtcbin, "latency", encoder->mOptions.latency, NULL);   // this doesn't seem to have an impact?
	
		// set latency on the rtpbin (https://github.com/centricular/gstwebrtc-demos/issues/102#issuecomment-575157321)
		GstElement* rtpbin = gst_bin_get_by_name(GST_BIN(peer_context->webrtcbin), "rtpbin");
		g_assert_nonnull(rtpbin);
		g_object_set(rtpbin, "latency", encoder->mOptions.latency, NULL);
		gst_object_unref(rtpbin);
		
		// add queue and webrtcbin elements to the pipeline
		gst_bin_add_many(GST_BIN(encoder->mPipeline), peer_context->queue, peer_context->webrtcbin, NULL);
		
		// link the queue to webrtc bin
		GstPad* srcpad = gst_element_get_static_pad(peer_context->queue, "src");
		g_assert_nonnull(srcpad);
		GstPad* sinkpad = gst_element_get_request_pad(peer_context->webrtcbin, "sink_%u");
		g_assert_nonnull(sinkpad);
		int ret = gst_pad_link(srcpad, sinkpad);
		g_assert_cmpint(ret, ==, GST_PAD_LINK_OK);
		gst_object_unref(srcpad);
		gst_object_unref(sinkpad);
		
		// link the queue to the tee
		GstElement* tee = gst_bin_get_by_name(GST_BIN(encoder->mPipeline), "videotee");
		g_assert_nonnull(tee);
		srcpad = gst_element_get_request_pad(tee, "src_%u");
		g_assert_nonnull(srcpad);
		gst_object_unref(tee);
		sinkpad = gst_element_get_static_pad(peer_context->queue, "sink");
		g_assert_nonnull(sinkpad);
		ret = gst_pad_link(srcpad, sinkpad);
		g_assert_cmpint(ret, ==, GST_PAD_LINK_OK);
		gst_object_unref(srcpad);
		gst_object_unref(sinkpad);
		
		// set transciever to send-only mode
		GArray* transceivers = NULL;
		
		g_signal_emit_by_name(peer_context->webrtcbin, "get-transceivers", &transceivers);
		g_assert(transceivers != NULL && transceivers->len > 0);
		
		GstWebRTCRTPTransceiver* transceiver = g_array_index(transceivers, GstWebRTCRTPTransceiver*, 0);
		g_object_set(transceiver, "direction", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY, NULL);
		g_array_unref(transceivers);
		
		// subscribe to callbacks
		g_signal_connect(peer_context->webrtcbin, "on-negotiation-needed", G_CALLBACK(gstWebRTC::onNegotiationNeeded), peer);
		g_signal_connect(peer_context->webrtcbin, "on-ice-candidate", G_CALLBACK(gstWebRTC::onIceCandidate), peer);
		
		// Set to pipeline branch to PLAYING
		ret = gst_element_sync_state_with_parent(peer_context->queue);
		g_assert_true(ret);
		ret = gst_element_sync_state_with_parent(peer_context->webrtcbin);
		g_assert_true(ret);
		
		return;
	}
	else if( peer->flags & WEBRTC_PEER_CLOSED )
	{
		LogVerbose(LOG_WEBRTC "WebRTC peer disconnected (%s, peer_id=%u)\n", peer->ip_address.c_str(), peer->ID);
		
		// remove webrtcbin from pipeline
		gst_bin_remove(GST_BIN(encoder->mPipeline), peer_context->webrtcbin);
		gst_element_set_state(peer_context->webrtcbin, GST_STATE_NULL);
		gst_object_unref(peer_context->webrtcbin);

		// disconnect queue pads
		GstPad* sinkpad = gst_element_get_static_pad(peer_context->queue, "sink");
		g_assert_nonnull(sinkpad);
		GstPad* srcpad = gst_pad_get_peer(sinkpad);
		g_assert_nonnull(srcpad);
		gst_object_unref(sinkpad);
  
		// remove queue from pipeline
		gst_bin_remove(GST_BIN(encoder->mPipeline), peer_context->queue);
		gst_element_set_state(peer_context->queue, GST_STATE_NULL);
		gst_object_unref(peer_context->queue);

		// free encoder-specific context
		delete peer_context;
		peer->user_data = NULL;
		
		return;
	}
	
	gstWebRTC::onWebsocketMessage(peer, message, message_size, user_data);
}
