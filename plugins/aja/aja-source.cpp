#include "aja-card-manager.hpp"
#include "aja-common.hpp"
#include "aja-ui-props.hpp"
#include "aja-source.hpp"
#include "aja-routing.hpp"

#include <util/threading.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <obs-module.h>

#include <ajantv2/includes/ntv2card.h>
#include <ajantv2/includes/ntv2utils.h>

#define NTV2_AUDIOSIZE_MAX (401 * 1024)

AJASource::AJASource(obs_source_t *source)
	: mVideoBuffer{},
	  mAudioBuffer{},
	  mCard{nullptr},
	  mSourceName{""},
	  mCardID{""},
	  mDeviceIndex{0},
	  mBuffering{false},
	  mIsCapturing{false},
	  mSourceProps{},
	  mTestPattern{},
	  mCaptureThread{nullptr},
	  mMutex{},
	  mSource{source}
{
}

AJASource::~AJASource()
{
	Deactivate();
	mTestPattern.clear();
	mVideoBuffer.Deallocate();
	mAudioBuffer.Deallocate();
	mVideoBuffer = NULL;
	mAudioBuffer = NULL;
}

void AJASource::SetCard(CNTV2Card *card)
{
	mCard = card;
}

CNTV2Card *AJASource::GetCard()
{
	return mCard;
}

void AJASource::SetOBSSource(obs_source_t *source)
{
	mSource = source;
}

obs_source_t *AJASource::GetOBSSource(void) const
{
	return mSource;
}

void AJASource::SetName(const std::string &name)
{
	mSourceName = name;
}

std::string AJASource::GetName() const
{
	return mSourceName;
}

void populate_source_device_list(obs_property_t *list)
{
	obs_property_list_clear(list);
	auto &cardManager = aja::CardManager::Instance();
	cardManager.EnumerateCards();
	for (const auto &iter : cardManager.GetCardEntries()) {
		if (iter.second) {
			CNTV2Card *card = iter.second->GetCard();
			if (!card)
				continue;

			if (aja::IsOutputOnlyDevice(iter.second->GetDeviceID()))
				continue;

			obs_property_list_add_string(
				list, iter.second->GetDisplayName().c_str(),
				iter.second->GetCardID().c_str());
		}
	}
}

//
// Capture Thread stuff
//

struct AudioOffsets {
	ULWord currentAddress = 0;
	ULWord lastAddress = 0;
	ULWord readOffset = 0;
	ULWord wrapAddress = 0;
	ULWord bytesRead = 0;
};

static void ResetAudioBufferOffsets(CNTV2Card *card,
				    NTV2AudioSystem audioSystem,
				    AudioOffsets &offsets)
{
	if (!card)
		return;

	offsets.currentAddress = 0;
	offsets.lastAddress = 0;
	offsets.readOffset = 0;
	offsets.wrapAddress = 0;
	offsets.bytesRead = 0;
	card->GetAudioReadOffset(offsets.readOffset, audioSystem);
	card->GetAudioWrapAddress(offsets.wrapAddress, audioSystem);
	offsets.wrapAddress += offsets.readOffset;
	offsets.lastAddress = offsets.readOffset;
}

void AJASource::GenerateTestPattern(NTV2VideoFormat vf, NTV2PixelFormat pf,
				    NTV2TestPatternSelect ps)
{
	NTV2VideoFormat vid_fmt = vf;
	NTV2PixelFormat pix_fmt = pf;
	if (vid_fmt == NTV2_FORMAT_UNKNOWN)
		vid_fmt = NTV2_FORMAT_720p_5994;
	if (pix_fmt == NTV2_FBF_INVALID)
		pix_fmt = kDefaultAJAPixelFormat;

	NTV2FormatDesc fd(vid_fmt, pix_fmt, NTV2_VANCMODE_OFF);
	auto bufSize = fd.GetTotalRasterBytes();
	if (bufSize != mTestPattern.size()) {
		mTestPattern.clear();
		mTestPattern.resize(bufSize);
		NTV2TestPatternGen gen;
		gen.DrawTestPattern(ps, fd.GetRasterWidth(),
				    fd.GetRasterHeight(), pix_fmt,
				    mTestPattern);
	}
	if (mTestPattern.size() == 0) {
		blog(LOG_DEBUG,
		     "AJASource::GenerateTestPattern: Error generating test pattern!");
		return;
	}

	struct obs_source_frame2 obsFrame;
	obsFrame.flip = false;
	obsFrame.timestamp = os_gettime_ns();
	obsFrame.width = fd.GetRasterWidth();
	obsFrame.height = fd.GetRasterHeight();
	obsFrame.format = aja::AJAPixelFormatToOBSVideoFormat(pix_fmt);
	obsFrame.data[0] = mTestPattern.data();
	obsFrame.linesize[0] = fd.GetBytesPerRow();
	video_format_get_parameters(VIDEO_CS_DEFAULT, VIDEO_RANGE_FULL,
				    obsFrame.color_matrix,
				    obsFrame.color_range_min,
				    obsFrame.color_range_max);
	obs_source_output_video2(mSource, &obsFrame);
	blog(LOG_DEBUG, "AJASource::GenerateTestPattern: Black");
}

void AJASource::CaptureThread(AJAThread *thread, void *data)
{
	UNUSED_PARAMETER(thread);

	auto ajaSource = (AJASource *)data;
	if (!ajaSource) {
		blog(LOG_WARNING,
		     "AJASource::CaptureThread: Plugin instance is null!");
		return;
	}

	blog(LOG_INFO,
	     "AJASource::CaptureThread: Starting capture thread for AJA source %s",
	     ajaSource->GetName().c_str());

	auto card = ajaSource->GetCard();
	if (!card) {
		blog(LOG_ERROR,
		     "AJASource::CaptureThread: Card instance is null!");
		return;
	}

	auto sourceProps = ajaSource->GetSourceProps();
	ajaSource->ResetVideoBuffer(sourceProps.videoFormat,
				    sourceProps.pixelFormat);
	auto inputSource = sourceProps.InitialInputSource();
	auto channel = sourceProps.Channel();
	auto audioSystem = sourceProps.AudioSystem();
	// Current "on-air" frame on the card. The capture thread "Ping-pongs" between
	// two frames, starting at an index corresponding to the framestore channel.
	// For example:
	// Channel 1 (index 0) = frames 0/1
	// Channel 2 (index 1) = frames 2/3
	// Channel 3 (index 2) = frames 4/5
	// Channel 4 (index 3) = frames 6/7
	// etc...
	ULWord currentCardFrame = (uint32_t)channel * 2;
	card->WaitForInputFieldID(NTV2_FIELD0, channel);

	currentCardFrame ^= 1;

	card->SetInputFrame(channel, currentCardFrame);

	AudioOffsets offsets;
	ResetAudioBufferOffsets(card, audioSystem, offsets);

	obs_data_t *settings = obs_source_get_settings(ajaSource->mSource);

	while (ajaSource->IsCapturing()) {
		if (card->GetModelName() == "(Not Found)") {
			os_sleep_ms(250);
			obs_source_update(ajaSource->mSource, settings);
			break;
		}

		auto videoFormat = sourceProps.videoFormat;
		auto pixelFormat = sourceProps.pixelFormat;
		auto ioSelection = sourceProps.ioSelect;
		bool audioOverrun = false;

		card->WaitForInputFieldID(NTV2_FIELD0, channel);
		currentCardFrame ^= 1;

		// Card format detection -- restarts capture thread via aja_source_update callback
		auto newVideoFormat = card->GetInputVideoFormat(
			inputSource, aja::Is3GLevelB(card, channel));
		if (newVideoFormat == NTV2_FORMAT_UNKNOWN) {
			blog(LOG_DEBUG,
			     "AJASource::CaptureThread: Video format unknown!");
			ajaSource->GenerateTestPattern(videoFormat, pixelFormat,
						       NTV2_TestPatt_Black);
			os_sleep_ms(250);
			continue;
		}

		newVideoFormat = aja::HandleSpecialCaseFormats(
			ioSelection, newVideoFormat, sourceProps.deviceID);

		if (sourceProps.autoDetect && (videoFormat != newVideoFormat)) {
			blog(LOG_INFO,
			     "AJASource::CaptureThread: New Video Format detected! Triggering 'aja_source_update' callback and returning...");
			blog(LOG_INFO,
			     "AJASource::CaptureThread: Current Video Format: %s, | Want Video Format: %s",
			     NTV2VideoFormatToString(videoFormat, true).c_str(),
			     NTV2VideoFormatToString(newVideoFormat, true)
				     .c_str());
			os_sleep_ms(250);
			obs_source_update(ajaSource->mSource, settings);
			break;
		}

		card->ReadAudioLastIn(offsets.currentAddress, audioSystem);
		offsets.currentAddress &= ~0x3; // Force DWORD alignment
		offsets.currentAddress += offsets.readOffset;
		if (offsets.currentAddress < offsets.lastAddress) {
			offsets.bytesRead =
				offsets.wrapAddress - offsets.lastAddress;

			if (offsets.bytesRead >
			    ajaSource->mAudioBuffer.GetByteCount()) {
				blog(LOG_DEBUG,
				     "AJASource::CaptureThread: Audio overrun (1)! Buffer Size: %d, Bytes Captured: %d",
				     ajaSource->mAudioBuffer.GetByteCount(),
				     offsets.bytesRead);
				ResetAudioBufferOffsets(card, audioSystem,
							offsets);
				audioOverrun = true;
			}

			if (!audioOverrun) {
				card->DMAReadAudio(audioSystem,
						   ajaSource->mAudioBuffer,
						   offsets.lastAddress,
						   offsets.bytesRead);
				card->DMAReadAudio(
					audioSystem,
					reinterpret_cast<ULWord *>(
						ajaSource->mAudioBuffer
							.GetHostAddress(
								offsets.bytesRead)),
					offsets.readOffset,
					offsets.currentAddress -
						offsets.readOffset);
				offsets.bytesRead += offsets.currentAddress -
						     offsets.readOffset;
			}

			if (offsets.bytesRead >
			    ajaSource->mAudioBuffer.GetByteCount()) {
				blog(LOG_DEBUG,
				     "AJASource::CaptureThread: Audio overrun (2)! Buffer Size: %d, Bytes Captured: %d",
				     ajaSource->mAudioBuffer.GetByteCount(),
				     offsets.bytesRead);
				ResetAudioBufferOffsets(card, audioSystem,
							offsets);
				audioOverrun = true;
			}
		} else {
			offsets.bytesRead =
				offsets.currentAddress - offsets.lastAddress;
			if (offsets.bytesRead >
			    ajaSource->mAudioBuffer.GetByteCount()) {
				blog(LOG_DEBUG,
				     "AJASource::CaptureThread: Audio overrun (3)! Buffer Size: %d, Bytes Captured: %d",
				     ajaSource->mAudioBuffer.GetByteCount(),
				     offsets.bytesRead);
				ResetAudioBufferOffsets(card, audioSystem,
							offsets);
				audioOverrun = true;
			}
			if (!audioOverrun) {
				card->DMAReadAudio(audioSystem,
						   ajaSource->mAudioBuffer,
						   offsets.lastAddress,
						   offsets.bytesRead);
			}
		}

		if (!audioOverrun) {
			offsets.lastAddress = offsets.currentAddress;
			obs_source_audio audioPacket;
			audioPacket.samples_per_sec = 48000;
			audioPacket.format = AUDIO_FORMAT_32BIT;
			audioPacket.speakers = SPEAKERS_7POINT1;
			audioPacket.frames = offsets.bytesRead / 32;
			audioPacket.timestamp = os_gettime_ns();
			audioPacket.data[0] = (uint8_t *)ajaSource->mAudioBuffer
						      .GetHostPointer();
			obs_source_output_audio(ajaSource->mSource,
						&audioPacket);
		}

		if (ajaSource->mVideoBuffer.GetByteCount() == 0) {
			blog(LOG_DEBUG,
			     "AJASource::CaptureThread: 0 bytes in video buffer! Something went wrong!");
			continue;
		}

		card->DMAReadFrame(currentCardFrame, ajaSource->mVideoBuffer,
				   ajaSource->mVideoBuffer.GetByteCount());

		auto actualVideoFormat = videoFormat;
		if (aja::Is3GLevelB(card, channel))
			actualVideoFormat = aja::GetLevelAFormatForLevelBFormat(
				videoFormat);

		NTV2FormatDesc fd(actualVideoFormat, pixelFormat);
		struct obs_source_frame2 obsFrame;
		obsFrame.flip = false;
		obsFrame.timestamp = os_gettime_ns();
		obsFrame.width = fd.GetRasterWidth();
		obsFrame.height = fd.GetRasterHeight();
		obsFrame.format = aja::AJAPixelFormatToOBSVideoFormat(
			sourceProps.pixelFormat);
		obsFrame.data[0] = reinterpret_cast<uint8_t *>(
			(ULWord *)ajaSource->mVideoBuffer.GetHostPointer());
		obsFrame.linesize[0] = fd.GetBytesPerRow();

		video_format_get_parameters(VIDEO_CS_DEFAULT, VIDEO_RANGE_FULL,
					    obsFrame.color_matrix,
					    obsFrame.color_range_min,
					    obsFrame.color_range_max);

		obs_source_output_video2(ajaSource->mSource, &obsFrame);

		card->SetInputFrame(channel, currentCardFrame);
	}

	blog(LOG_INFO, "AJASource::Capturethread: Thread loop stopped");

	ajaSource->GenerateTestPattern(sourceProps.videoFormat,
				       sourceProps.pixelFormat,
				       NTV2_TestPatt_Black);

	obs_data_release(settings);
}

void AJASource::Deactivate()
{
	SetCapturing(false);

	if (mCaptureThread) {
		if (mCaptureThread->Active()) {
			mCaptureThread->Stop();
			blog(LOG_INFO, "AJASource::CaptureThread: Stopped!");
		}
		delete mCaptureThread;
		mCaptureThread = nullptr;
		blog(LOG_INFO, "AJASource::CaptureThread: Destroyed!");
	}
}

void AJASource::Activate(bool enable)
{
	if (mCaptureThread == nullptr) {
		mCaptureThread = new AJAThread();
		mCaptureThread->Attach(AJASource::CaptureThread, this);
		mCaptureThread->SetPriority(AJA_ThreadPriority_High);
		blog(LOG_INFO, "AJASource::CaptureThread: Created!");
	}

	if (enable) {
		SetCapturing(true);
		if (!mCaptureThread->Active()) {
			mCaptureThread->Start();
			blog(LOG_INFO, "AJASource::CaptureThread: Started!");
		}
	}
}

bool AJASource::IsCapturing() const
{
	return mIsCapturing;
}

void AJASource::SetCapturing(bool capturing)
{
	std::lock_guard<std::mutex> lock(mMutex);
	mIsCapturing = capturing;
}

//
// CardEntry/Device stuff
//
std::string AJASource::CardID() const
{
	return mCardID;
}
void AJASource::SetCardID(const std::string &cardID)
{
	mCardID = cardID;
}
uint32_t AJASource::DeviceIndex() const
{
	return static_cast<uint32_t>(mDeviceIndex);
}
void AJASource::SetDeviceIndex(uint32_t index)
{
	mDeviceIndex = static_cast<UWord>(index);
}

//
// AJASource Properties stuff
//
void AJASource::SetSourceProps(const SourceProps &props)
{
	mSourceProps = props;
}

SourceProps AJASource::GetSourceProps() const
{
	return mSourceProps;
}

bool AJASource::ReadChannelVPIDs(NTV2Channel channel, VPIDData &vpids)
{
	ULWord vpid_a = 0;
	ULWord vpid_b = 0;
	bool read_ok = mCard->ReadSDIInVPID(channel, vpid_a, vpid_b);
	vpids.SetA(vpid_a);
	vpids.SetB(vpid_b);
	vpids.Parse();
	return read_ok;
}

bool AJASource::ReadWireFormats(NTV2DeviceID device_id, IOSelection io_select,
				NTV2VideoFormat &vf, NTV2PixelFormat &pf,
				VPIDDataList &vpids)
{
	NTV2InputSourceSet input_srcs;
	aja::IOSelectionToInputSources(io_select, input_srcs);
	if (input_srcs.empty()) {
		blog(LOG_INFO,
		     "AJASource::ReadWireFormats: No NTV2InputSources found for IOSelection %s",
		     aja::IOSelectionToString(io_select).c_str());
		return false;
	}

	NTV2InputSource initial_src = *input_srcs.begin();
	for (auto &&src : input_srcs) {
		auto channel = NTV2InputSourceToChannel(src);
		mCard->EnableChannel(channel);
		if (NTV2_INPUT_SOURCE_IS_SDI(src)) {
			if (NTV2DeviceHasBiDirectionalSDI(device_id)) {
				mCard->SetSDITransmitEnable(channel, false);
			}
			mCard->WaitForInputVerticalInterrupt(channel);
			VPIDData vpid_data;
			if (ReadChannelVPIDs(channel, vpid_data))
				vpids.push_back(vpid_data);
		} else if (NTV2_INPUT_SOURCE_IS_HDMI(src)) {
			mCard->WaitForInputVerticalInterrupt(channel);

			ULWord hdmi_version =
				NTV2DeviceGetHDMIVersion(device_id);
			// HDMIv1 handles its own RGB->YCbCr color space conversion
			if (hdmi_version == 1) {
				pf = kDefaultAJAPixelFormat;
			} else {
				NTV2LHIHDMIColorSpace hdmiInputColor;
				mCard->GetHDMIInputColor(hdmiInputColor,
							 channel);
				if (hdmiInputColor ==
				    NTV2_LHIHDMIColorSpaceYCbCr) {
					pf = kDefaultAJAPixelFormat;
				} else if (hdmiInputColor ==
					   NTV2_LHIHDMIColorSpaceRGB) {
					pf = NTV2_FBF_24BIT_BGR;
				}
			}
		}
	}

	NTV2Channel initial_channel = NTV2InputSourceToChannel(initial_src);
	mCard->WaitForInputVerticalInterrupt(initial_channel);

	vf = mCard->GetInputVideoFormat(
		initial_src, aja::Is3GLevelB(mCard, initial_channel));

	if (NTV2_INPUT_SOURCE_IS_SDI(initial_src)) {
		if (vpids.size() > 0) {
			auto vpid = *vpids.begin();
			if (vpid.Sampling() == VPIDSampling_YUV_422) {
				pf = NTV2_FBF_8BIT_YCBCR;
				blog(LOG_INFO,
				     "AJASource::ReadWireFormats - Detected pixel format %s",
				     NTV2FrameBufferFormatToString(pf, true)
					     .c_str());
			} else if (vpid.Sampling() == VPIDSampling_GBR_444) {
				pf = NTV2_FBF_24BIT_BGR;
				blog(LOG_INFO,
				     "AJASource::ReadWireFormats - Detected pixel format %s",
				     NTV2FrameBufferFormatToString(pf, true)
					     .c_str());
			}
		}
	}

	vf = aja::HandleSpecialCaseFormats(io_select, vf, device_id);

	blog(LOG_INFO, "AJASource::ReadWireFormats - Detected video format %s",
	     NTV2VideoFormatToString(vf).c_str());

	return true;
}

void AJASource::ResetVideoBuffer(NTV2VideoFormat vf, NTV2PixelFormat pf)
{
	if (vf != NTV2_FORMAT_UNKNOWN) {
		auto videoBufferSize = GetVideoWriteSize(vf, pf);

		if (mVideoBuffer)
			mVideoBuffer.Deallocate();

		mVideoBuffer.Allocate(videoBufferSize, true);

		blog(LOG_INFO,
		     "AJASource::ResetVideoBuffer: Video Format: %s | Pixel Format: %s | Buffer Size: %d",
		     NTV2VideoFormatToString(vf, false).c_str(),
		     NTV2FrameBufferFormatToString(pf, true).c_str(),
		     videoBufferSize);
	}
}

void AJASource::ResetAudioBuffer(size_t size)
{
	if (mAudioBuffer)
		mAudioBuffer.Deallocate();
	mAudioBuffer.Allocate(size, true);
}

static const char *aja_source_get_name(void *);
static void *aja_source_create(obs_data_t *, obs_source_t *);
static void aja_source_destroy(void *);
static void aja_source_activate(void *);
static void aja_source_deactivate(void *);
static void aja_source_update(void *, obs_data_t *);

const char *aja_source_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text(kUIPropCaptureModule.text);
}

bool aja_source_device_changed(void *data, obs_properties_t *props,
			       obs_property_t *list, obs_data_t *settings)
{
	UNUSED_PARAMETER(list);

	blog(LOG_DEBUG, "AJA Source Device Changed");

	auto *ajaSource = (AJASource *)data;
	if (!ajaSource) {
		blog(LOG_DEBUG,
		     "aja_source_device_changed: AJA Source instance is null!");
		return false;
	}

	const char *cardID = obs_data_get_string(settings, kUIPropDevice.id);
	if (!cardID || !cardID[0])
		return false;

	auto &cardManager = aja::CardManager::Instance();
	auto cardEntry = cardManager.GetCardEntry(cardID);
	if (!cardEntry) {
		blog(LOG_DEBUG,
		     "aja_source_device_changed: Card Entry not found for %s",
		     cardID);
		return false;
	}
	blog(LOG_DEBUG, "Found CardEntry for %s", cardID);
	CNTV2Card *card = cardEntry->GetCard();
	if (!card) {
		blog(LOG_DEBUG,
		     "aja_source_device_changed: Card instance is null!");
		return false;
	}

	const NTV2DeviceID deviceID = card->GetDeviceID();

	/* If Channel 1 is actively in use, filter the video format list to only
	 * show video formats within the same framerate family. If Channel 1 is
	 * not active we just go ahead and try to set all framestores to the same video format.
	 * This is because Channel 1's clock rate will govern the card's Free Run clock.
	 */
	NTV2VideoFormat videoFormatChannel1 = NTV2_FORMAT_UNKNOWN;
	if (!cardEntry->ChannelReady(NTV2_CHANNEL1, ajaSource->GetName())) {
		card->GetVideoFormat(videoFormatChannel1, NTV2_CHANNEL1);
	}

	obs_property_t *devices_list =
		obs_properties_get(props, kUIPropDevice.id);
	obs_property_t *io_select_list =
		obs_properties_get(props, kUIPropInput.id);
	obs_property_t *vid_fmt_list =
		obs_properties_get(props, kUIPropVideoFormatSelect.id);
	obs_property_t *pix_fmt_list =
		obs_properties_get(props, kUIPropPixelFormatSelect.id);
	obs_property_t *sdi_trx_list =
		obs_properties_get(props, kUIPropSDITransport.id);
	obs_property_t *sdi_4k_list =
		obs_properties_get(props, kUIPropSDITransport4K.id);

	obs_property_list_clear(vid_fmt_list);
	obs_property_list_add_int(vid_fmt_list, obs_module_text("Auto"),
				  kAutoDetect);
	populate_video_format_list(deviceID, vid_fmt_list, videoFormatChannel1);

	obs_property_list_clear(pix_fmt_list);
	obs_property_list_add_int(pix_fmt_list, obs_module_text("Auto"),
				  kAutoDetect);
	populate_pixel_format_list(deviceID, pix_fmt_list);

	IOSelection io_select = static_cast<IOSelection>(
		obs_data_get_int(settings, kUIPropInput.id));
	obs_property_list_clear(sdi_trx_list);
	obs_property_list_add_int(sdi_trx_list, obs_module_text("Auto"),
				  kAutoDetect);
	populate_sdi_transport_list(sdi_trx_list, io_select);

	obs_property_list_clear(sdi_4k_list);
	populate_sdi_4k_transport_list(sdi_4k_list);

	populate_io_selection_input_list(cardID, ajaSource->GetName(), deviceID,
					 io_select_list);

	auto curr_vf = static_cast<NTV2VideoFormat>(
		obs_data_get_int(settings, kUIPropVideoFormatSelect.id));

	bool have_cards = cardManager.NumCardEntries() > 0;
	obs_property_set_visible(devices_list, have_cards);
	obs_property_set_visible(io_select_list, have_cards);
	obs_property_set_visible(vid_fmt_list, have_cards);
	obs_property_set_visible(pix_fmt_list, have_cards);
	obs_property_set_visible(
		sdi_4k_list, have_cards && NTV2_IS_4K_VIDEO_FORMAT(curr_vf));

	return true;
}

bool aja_io_selection_changed(void *data, obs_properties_t *props,
			      obs_property_t *list, obs_data_t *settings)
{
	UNUSED_PARAMETER(list);

	AJASource *ajaSource = (AJASource *)data;
	if (!ajaSource) {
		blog(LOG_DEBUG,
		     "aja_io_selection_changed: AJA Source instance is null!");
		return false;
	}

	const char *cardID = obs_data_get_string(settings, kUIPropDevice.id);
	if (!cardID || !cardID[0])
		return false;

	auto &cardManager = aja::CardManager::Instance();
	auto cardEntry = cardManager.GetCardEntry(cardID);
	if (!cardEntry) {
		blog(LOG_DEBUG,
		     "aja_io_selection_changed: Card Entry not found for %s",
		     cardID);
		return false;
	}

	obs_property_t *io_select_list =
		obs_properties_get(props, kUIPropInput.id);

	filter_io_selection_input_list(cardID, ajaSource->GetName(),
				       io_select_list);

	return true;
}

bool aja_sdi_mode_list_changed(obs_properties_t *props, obs_property_t *list,
			       obs_data_t *settings)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(list);
	UNUSED_PARAMETER(settings);

	return true;
}

void *aja_source_create(obs_data_t *settings, obs_source_t *source)
{
	blog(LOG_DEBUG, "AJA Source Create");

	auto ajaSource = new AJASource(source);

	ajaSource->SetName(obs_source_get_name(source));

	obs_source_set_async_decoupled(source, true);

	ajaSource->SetOBSSource(source);
	ajaSource->ResetAudioBuffer(NTV2_AUDIOSIZE_MAX);
	ajaSource->Activate(false);

	obs_source_update(source, settings);

	return ajaSource;
}

void aja_source_destroy(void *data)
{
	blog(LOG_DEBUG, "AJA Source Destroy");

	auto ajaSource = (AJASource *)data;
	if (!ajaSource) {
		blog(LOG_ERROR, "aja_source_destroy: Plugin instance is null!");
		return;
	}

	ajaSource->Deactivate();

	NTV2DeviceID deviceID = DEVICE_ID_NOTFOUND;
	CNTV2Card *card = ajaSource->GetCard();
	if (card) {
		deviceID = card->GetDeviceID();
		aja::Routing::StopSourceAudio(ajaSource->GetSourceProps(),
					      card);
	}

	ajaSource->mVideoBuffer.Deallocate();
	ajaSource->mAudioBuffer.Deallocate();
	ajaSource->mVideoBuffer = NULL;
	ajaSource->mAudioBuffer = NULL;

	auto &cardManager = aja::CardManager::Instance();
	const auto &cardID = ajaSource->CardID();
	auto cardEntry = cardManager.GetCardEntry(cardID);
	if (!cardEntry) {
		blog(LOG_DEBUG,
		     "aja_source_destroy: Card Entry not found for %s",
		     cardID.c_str());
		return;
	}

	auto ioSelect = ajaSource->GetSourceProps().ioSelect;
	if (!cardEntry->ReleaseInputSelection(ioSelect, deviceID,
					      ajaSource->GetName())) {
		blog(LOG_WARNING,
		     "aja_source_destroy: Error releasing Input Selection!");
	}

	delete ajaSource;
	ajaSource = nullptr;
}

static void aja_source_show(void *data)
{
	auto ajaSource = (AJASource *)data;
	if (!ajaSource) {
		blog(LOG_ERROR,
		     "aja_source_show: AJA Source instance is null!");
		return;
	}

	bool deactivateWhileNotShowing =
		ajaSource->GetSourceProps().deactivateWhileNotShowing;
	bool showing = obs_source_showing(ajaSource->GetOBSSource());
	blog(LOG_DEBUG,
	     "aja_source_show: deactivateWhileNotShowing = %s, showing = %s",
	     deactivateWhileNotShowing ? "true" : "false",
	     showing ? "true" : "false");
	if (deactivateWhileNotShowing && showing && !ajaSource->IsCapturing()) {
		ajaSource->Activate(true);
		blog(LOG_DEBUG, "aja_source_show: activated capture thread!");
	}
}

static void aja_source_hide(void *data)
{
	auto ajaSource = (AJASource *)data;
	if (!ajaSource)
		return;

	bool deactivateWhileNotShowing =
		ajaSource->GetSourceProps().deactivateWhileNotShowing;
	bool showing = obs_source_showing(ajaSource->GetOBSSource());
	blog(LOG_DEBUG,
	     "aja_source_hide: deactivateWhileNotShowing = %s, showing = %s",
	     deactivateWhileNotShowing ? "true" : "false",
	     showing ? "true" : "false");
	if (deactivateWhileNotShowing && !showing && ajaSource->IsCapturing()) {
		ajaSource->Deactivate();
		blog(LOG_DEBUG, "aja_source_hide: deactivated capture thread!");
	}
}

static void aja_source_activate(void *data)
{
	UNUSED_PARAMETER(data);
}

static void aja_source_deactivate(void *data)
{
	UNUSED_PARAMETER(data);
}

static void aja_source_update(void *data, obs_data_t *settings)
{
	static bool initialized = false;

	auto ajaSource = (AJASource *)data;
	if (!ajaSource) {
		blog(LOG_WARNING,
		     "aja_source_update: Plugin instance is null!");
		return;
	}

	auto io_select = static_cast<IOSelection>(
		obs_data_get_int(settings, kUIPropInput.id));
	auto vf_select = static_cast<NTV2VideoFormat>(
		obs_data_get_int(settings, kUIPropVideoFormatSelect.id));
	auto pf_select = static_cast<NTV2PixelFormat>(
		obs_data_get_int(settings, kUIPropPixelFormatSelect.id));
	auto sdi_trx_select = static_cast<SDITransport>(
		obs_data_get_int(settings, kUIPropSDITransport.id));
	auto sdi_t4k_select = static_cast<SDITransport4K>(
		obs_data_get_int(settings, kUIPropSDITransport4K.id));
	bool deactivateWhileNotShowing =
		obs_data_get_bool(settings, kUIPropDeactivateWhenNotShowing.id);
	const std::string &wantCardID =
		obs_data_get_string(settings, kUIPropDevice.id);

	const std::string &currentCardID = ajaSource->CardID();
	if (wantCardID != currentCardID) {
		initialized = false;
		ajaSource->Deactivate();
	}

	auto &cardManager = aja::CardManager::Instance();
	cardManager.EnumerateCards();
	auto cardEntry = cardManager.GetCardEntry(wantCardID);
	if (!cardEntry) {
		blog(LOG_DEBUG,
		     "aja_source_update: Card Entry not found for %s",
		     wantCardID.c_str());
		return;
	}
	CNTV2Card *card = cardEntry->GetCard();
	if (!card || !card->IsOpen()) {
		blog(LOG_ERROR, "aja_source_update: AJA device %s not open!",
		     wantCardID.c_str());
		return;
	}
	if (card->GetModelName() == "(Not Found)") {
		blog(LOG_ERROR,
		     "aja_source_update: AJA device %s disconnected?",
		     wantCardID.c_str());
		return;
	}
	ajaSource->SetCard(cardEntry->GetCard());

	SourceProps curr_props = ajaSource->GetSourceProps();

	// Release Channels from previous card if card ID changes
	if (wantCardID != currentCardID) {
		auto prevCardEntry = cardManager.GetCardEntry(currentCardID);
		if (prevCardEntry) {
			const std::string &ioSelectStr =
				aja::IOSelectionToString(curr_props.ioSelect);
			if (!prevCardEntry->ReleaseInputSelection(
				    curr_props.ioSelect, curr_props.deviceID,
				    ajaSource->GetName())) {
				blog(LOG_WARNING,
				     "aja_source_update: Error releasing IOSelection %s for card ID %s",
				     ioSelectStr.c_str(),
				     currentCardID.c_str());
			} else {
				blog(LOG_INFO,
				     "aja_source_update: Released IOSelection %s for card ID %s",
				     ioSelectStr.c_str(),
				     currentCardID.c_str());
				ajaSource->SetCardID(wantCardID);
				io_select = IOSelection::Invalid;
			}
		}
	}

	if (io_select == IOSelection::Invalid) {
		blog(LOG_DEBUG, "aja_source_update: Invalid IOSelection");
		return;
	}

	SourceProps want_props;
	want_props.deviceID = card->GetDeviceID();
	want_props.ioSelect = io_select;
	want_props.videoFormat =
		((int32_t)vf_select == kAutoDetect)
			? NTV2_FORMAT_UNKNOWN
			: static_cast<NTV2VideoFormat>(vf_select);
	want_props.pixelFormat =
		((int32_t)pf_select == kAutoDetect)
			? NTV2_FBF_INVALID
			: static_cast<NTV2PixelFormat>(pf_select);
	want_props.sdiTransport =
		((int32_t)sdi_trx_select == kAutoDetect)
			? SDITransport::Unknown
			: static_cast<SDITransport>(sdi_trx_select);
	want_props.sdi4kTransport = sdi_t4k_select;
	want_props.vpids.clear();
	want_props.deactivateWhileNotShowing = deactivateWhileNotShowing;
	want_props.autoDetect = ((int32_t)vf_select == kAutoDetect ||
				 (int32_t)pf_select == kAutoDetect);
	ajaSource->SetCardID(wantCardID);
	ajaSource->SetDeviceIndex((UWord)cardEntry->GetCardIndex());

	if (NTV2_IS_4K_VIDEO_FORMAT(want_props.videoFormat) &&
	    want_props.sdi4kTransport == SDITransport4K::Squares) {
		if (want_props.ioSelect == IOSelection::SDI1_2) {
			want_props.ioSelect = IOSelection::SDI1_2_Squares;
		} else if (want_props.ioSelect == IOSelection::SDI3_4) {
			want_props.ioSelect = IOSelection::SDI3_4_Squares;
		}
	}

	// Release Channels if IOSelection changes
	if (want_props.ioSelect != curr_props.ioSelect) {
		const std::string &ioSelectStr =
			aja::IOSelectionToString(curr_props.ioSelect);
		if (!cardEntry->ReleaseInputSelection(curr_props.ioSelect,
						      curr_props.deviceID,
						      ajaSource->GetName())) {
			blog(LOG_WARNING,
			     "aja_source_update: Error releasing IOSelection %s for card ID %s",
			     ioSelectStr.c_str(), currentCardID.c_str());
		} else {
			blog(LOG_INFO,
			     "aja_source_update: Released IOSelection %s for card ID %s",
			     ioSelectStr.c_str(), currentCardID.c_str());
		}
	}

	// Acquire Channels for current IOSelection
	if (!cardEntry->AcquireInputSelection(want_props.ioSelect,
					      want_props.deviceID,
					      ajaSource->GetName())) {
		blog(LOG_ERROR,
		     "aja_source_update: Could not acquire IOSelection %s",
		     aja::IOSelectionToString(want_props.ioSelect).c_str());
		return;
	}

	// Read SDI video payload IDs (VPID) used for helping to determine the wire format
	NTV2VideoFormat new_vf = want_props.videoFormat;
	NTV2PixelFormat new_pf = want_props.pixelFormat;
	if (!ajaSource->ReadWireFormats(want_props.deviceID,
					want_props.ioSelect, new_vf, new_pf,
					want_props.vpids)) {
		blog(LOG_ERROR, "aja_source_update: ReadWireFormats failed!");
		cardEntry->ReleaseInputSelection(want_props.ioSelect,
						 curr_props.deviceID,
						 ajaSource->GetName());
		return;
	}

	// Set auto-detected formats
	if ((int32_t)vf_select == kAutoDetect)
		want_props.videoFormat = new_vf;
	if ((int32_t)pf_select == kAutoDetect)
		want_props.pixelFormat = new_pf;

	if (want_props.videoFormat == NTV2_FORMAT_UNKNOWN ||
	    want_props.pixelFormat == NTV2_FBF_INVALID) {
		blog(LOG_ERROR,
		     "aja_source_update: Unknown video/pixel format(s): %s / %s",
		     NTV2VideoFormatToString(want_props.videoFormat).c_str(),
		     NTV2FrameBufferFormatToString(want_props.pixelFormat)
			     .c_str());
		cardEntry->ReleaseInputSelection(want_props.ioSelect,
						 curr_props.deviceID,
						 ajaSource->GetName());
		return;
	}

	// Change capture format and restart capture thread
	if (!initialized || want_props != ajaSource->GetSourceProps()) {
		aja::Routing::ConfigureSourceRoute(want_props,
						   NTV2_MODE_CAPTURE, card);
		ajaSource->Deactivate();
		initialized = true;
	}

	ajaSource->SetSourceProps(want_props);
	aja::Routing::StartSourceAudio(want_props, card);
	card->SetReference(NTV2_REFERENCE_FREERUN);
	ajaSource->Activate(true);
}

static obs_properties_t *aja_source_get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *device_list = obs_properties_add_list(
		props, kUIPropDevice.id, obs_module_text(kUIPropDevice.text),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	populate_source_device_list(device_list);
	obs_property_t *io_select_list = obs_properties_add_list(
		props, kUIPropInput.id, obs_module_text(kUIPropInput.text),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_t *vid_fmt_list = obs_properties_add_list(
		props, kUIPropVideoFormatSelect.id,
		obs_module_text(kUIPropVideoFormatSelect.text),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_properties_add_list(props, kUIPropPixelFormatSelect.id,
				obs_module_text(kUIPropPixelFormatSelect.text),
				OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_properties_add_list(props, kUIPropSDITransport.id,
				obs_module_text(kUIPropSDITransport.text),
				OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_properties_add_list(props, kUIPropSDITransport4K.id,
				obs_module_text(kUIPropSDITransport4K.text),
				OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_properties_add_bool(
		props, kUIPropDeactivateWhenNotShowing.id,
		obs_module_text(kUIPropDeactivateWhenNotShowing.text));

	obs_property_set_modified_callback(vid_fmt_list,
					   aja_video_format_changed);
	obs_property_set_modified_callback2(device_list,
					    aja_source_device_changed, data);
	obs_property_set_modified_callback2(io_select_list,
					    aja_io_selection_changed, data);

	return props;
}

void aja_source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, kUIPropInput.id,
				 static_cast<long long>(IOSelection::Invalid));
	obs_data_set_default_int(settings, kUIPropVideoFormatSelect.id,
				 static_cast<long long>(kAutoDetect));
	obs_data_set_default_int(settings, kUIPropPixelFormatSelect.id,
				 static_cast<long long>(kAutoDetect));
	obs_data_set_default_int(settings, kUIPropSDITransport.id,
				 static_cast<long long>(kAutoDetect));
	obs_data_set_default_int(
		settings, kUIPropSDITransport4K.id,
		static_cast<long long>(SDITransport4K::TwoSampleInterleave));
	obs_data_set_default_bool(settings, kUIPropDeactivateWhenNotShowing.id,
				  false);
}

void aja_source_save(void *data, obs_data_t *settings)
{
	AJASource *ajaSource = (AJASource *)data;
	if (!ajaSource) {
		blog(LOG_ERROR,
		     "aja_source_save: AJA Source instance is null!");
		return;
	}

	const char *cardID = obs_data_get_string(settings, kUIPropDevice.id);
	if (!cardID || !cardID[0])
		return;

	auto &cardManager = aja::CardManager::Instance();
	auto cardEntry = cardManager.GetCardEntry(cardID);
	if (!cardEntry) {
		blog(LOG_DEBUG, "aja_source_save: Card Entry not found for %s",
		     cardID);
		return;
	}

	auto oldName = ajaSource->GetName();
	auto newName = obs_source_get_name(ajaSource->GetOBSSource());
	if (oldName != newName &&
	    cardEntry->UpdateChannelOwnerName(oldName, newName)) {
		ajaSource->SetName(newName);
		blog(LOG_DEBUG, "aja_source_save: Renamed \"%s\" to \"%s\"",
		     oldName.c_str(), newName);
	}
}

struct obs_source_info create_aja_source_info()
{
	struct obs_source_info aja_source_info = {};
	aja_source_info.id = kUIPropCaptureModule.id;
	aja_source_info.type = OBS_SOURCE_TYPE_INPUT;
	aja_source_info.output_flags = OBS_SOURCE_ASYNC_VIDEO |
				       OBS_SOURCE_AUDIO |
				       OBS_SOURCE_DO_NOT_DUPLICATE;
	aja_source_info.get_name = aja_source_get_name;
	aja_source_info.create = aja_source_create;
	aja_source_info.destroy = aja_source_destroy;
	aja_source_info.update = aja_source_update;
	aja_source_info.show = aja_source_show;
	aja_source_info.hide = aja_source_hide;
	aja_source_info.activate = aja_source_activate;
	aja_source_info.deactivate = aja_source_deactivate;
	aja_source_info.get_properties = aja_source_get_properties;
	aja_source_info.get_defaults = aja_source_get_defaults;
	aja_source_info.save = aja_source_save;
	aja_source_info.icon_type = OBS_ICON_TYPE_CAMERA;
	return aja_source_info;
}
