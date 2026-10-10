package main

// scanAnnexB 扫描一段 Annex B 码流，取出首个 SPS / PPS，并判断是否含 IDR 帧。
// 原始数据不做改动，直接交给浏览器侧的 WebCodecs。
func scanAnnexB(data []byte) (sps, pps []byte, keyframe bool) {
	index := 0
	for index+3 < len(data) {
		startLength := 0
		if data[index] == 0 && data[index+1] == 0 {
			switch {
			case data[index+2] == 1:
				startLength = 3
			case index+3 < len(data) && data[index+2] == 0 && data[index+3] == 1:
				startLength = 4
			}
		}
		if startLength == 0 {
			index++
			continue
		}

		nalStart := index + startLength
		nalEnd := nalStart
		for nalEnd+2 < len(data) {
			if data[nalEnd] == 0 && data[nalEnd+1] == 0 &&
				(data[nalEnd+2] == 1 || (nalEnd+3 < len(data) && data[nalEnd+2] == 0 && data[nalEnd+3] == 1)) {
				break
			}
			nalEnd++
		}
		if nalEnd > len(data) {
			nalEnd = len(data)
		}

		if nalStart < nalEnd {
			switch data[nalStart] & 0x1f {
			case 7:
				if sps == nil {
					sps = append([]byte(nil), data[nalStart:nalEnd]...)
				}
			case 8:
				if pps == nil {
					pps = append([]byte(nil), data[nalStart:nalEnd]...)
				}
			case 5:
				keyframe = true
			}
		}
		index = nalEnd
	}
	return sps, pps, keyframe
}

// buildAvcC 按 ISO/IEC 14496-15 组装 AVCDecoderConfigurationRecord。
// 浏览器侧的 avcCodecString 会读 record[1..3] 拼出 avc1.PPCCLL。
func buildAvcC(sps, pps []byte) []byte {
	if len(sps) < 4 || len(pps) == 0 {
		return nil
	}
	record := make([]byte, 0, 11+len(sps)+len(pps))
	record = append(record,
		1,            // configurationVersion
		sps[1],       // AVCProfileIndication
		sps[2],       // profile_compatibility
		sps[3],       // AVCLevelIndication
		0xFF,         // 6 位保留 + lengthSizeMinusOne = 3（4 字节长度前缀）
		0xE1,         // 3 位保留 + numOfSequenceParameterSets = 1
		byte(len(sps)>>8), byte(len(sps)),
	)
	record = append(record, sps...)
	record = append(record, 1, byte(len(pps)>>8), byte(len(pps)))
	record = append(record, pps...)
	return record
}
