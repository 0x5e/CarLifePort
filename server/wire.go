package main

import "encoding/binary"

// 浏览器侧的二进制帧格式。与 web/src/protocol.ts 的 parseWireMessage 一一对应，
// 全部小端。
//
//	type 1 VIDEO_CONFIG:  [1][streamType u8][codec u8][width u32][height u32][avcC]
//	type 2 VIDEO_FRAME:   [2][streamType u8][flags u8][pts u64][Annex B 数据]
//	type 3 AUDIO_CONFIG:  [3][streamType u8][codec u8][sampleRate u32][channels u8][nameLen u8][name][description]
//	type 4 AUDIO_PACKET:  [4][streamType u8][pts u64][数据]
//	type 6 AUDIO_STOPPED: [6][streamType u8]
const (
	wireVideoConfig  byte = 1
	wireVideoFrame   byte = 2
	wireAudioConfig  byte = 3
	wireAudioPacket  byte = 4
	wireScreenActive byte = 5
	wireAudioStopped byte = 6
)

const (
	wireVideoCodecH264 byte = 1
	wireAudioCodecLPCM byte = 3

	wireFlagKeyframe byte = 1
)

// CarLife 只有一路视频；音频按通道拆成媒体与导航播报两路，便于前端分别调音量。
const (
	videoStreamType  byte = 1
	audioStreamMedia byte = 1
	audioStreamTTS   byte = 2
)

const (
	streamNameMedia = "媒体"
	streamNameTTS   = "导航播报"
)

func encodeVideoConfig(streamType byte, width, height int, avcC []byte) []byte {
	frame := make([]byte, 11+len(avcC))
	frame[0] = wireVideoConfig
	frame[1] = streamType
	frame[2] = wireVideoCodecH264
	binary.LittleEndian.PutUint32(frame[3:], uint32(width))
	binary.LittleEndian.PutUint32(frame[7:], uint32(height))
	copy(frame[11:], avcC)
	return frame
}

func encodeVideoFrame(streamType byte, keyframe bool, annexB []byte) []byte {
	frame := make([]byte, 11+len(annexB))
	frame[0] = wireVideoFrame
	frame[1] = streamType
	if keyframe {
		frame[2] = wireFlagKeyframe
	}
	// 时间戳交给解码器自行按帧率推进，这里固定为 0。
	copy(frame[11:], annexB)
	return frame
}

func encodeAudioConfig(streamType byte, sampleRate, channels int, name string, description []byte) []byte {
	nameBytes := []byte(name)
	frame := make([]byte, 9+len(nameBytes)+len(description))
	frame[0] = wireAudioConfig
	frame[1] = streamType
	frame[2] = wireAudioCodecLPCM
	binary.LittleEndian.PutUint32(frame[3:], uint32(sampleRate))
	frame[7] = byte(channels)
	frame[8] = byte(len(nameBytes))
	copy(frame[9:], nameBytes)
	copy(frame[9+len(nameBytes):], description)
	return frame
}

func encodeAudioPacket(streamType byte, timestampUs uint64, payload []byte) []byte {
	frame := make([]byte, 10+len(payload))
	frame[0] = wireAudioPacket
	frame[1] = streamType
	binary.LittleEndian.PutUint64(frame[2:], timestampUs)
	copy(frame[10:], payload)
	return frame
}

func encodeAudioStopped(streamType byte) []byte {
	return []byte{wireAudioStopped, streamType}
}
