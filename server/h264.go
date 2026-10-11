package main

// scanAnnexB scans an Annex B bitstream, extracts the first SPS / PPS, and reports whether it contains an IDR frame.
// The raw data is left unchanged and passed directly to WebCodecs on the browser side.
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

// buildAvcC assembles an AVCDecoderConfigurationRecord per ISO/IEC 14496-15.
// avcCodecString on the browser side reads record[1..3] to build avc1.PPCCLL.
func buildAvcC(sps, pps []byte) []byte {
	if len(sps) < 4 || len(pps) == 0 {
		return nil
	}
	record := make([]byte, 0, 11+len(sps)+len(pps))
	record = append(record,
		1,      // configurationVersion
		sps[1], // AVCProfileIndication
		sps[2], // profile_compatibility
		sps[3], // AVCLevelIndication
		0xFF,   // 6 reserved bits + lengthSizeMinusOne = 3 (4-byte length prefix)
		0xE1,   // 3 reserved bits + numOfSequenceParameterSets = 1
		byte(len(sps)>>8), byte(len(sps)),
	)
	record = append(record, sps...)
	record = append(record, 1, byte(len(pps)>>8), byte(len(pps)))
	record = append(record, pps...)
	return record
}
