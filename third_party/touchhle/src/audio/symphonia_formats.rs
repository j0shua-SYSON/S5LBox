/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! Encoded MP3 packet access and decoding of miscellaneous audio formats.
//!
//! This should be the only module in touchHLE that makes use of [symphonia].
//! For AAC, Only the LC profile and MPEG-4 container format are supported (see
//! feature list in Cargo.toml).

use std::io::Cursor;
use symphonia::core::audio::AudioSpec;
use symphonia::core::codecs::audio::well_known::{
    CODEC_ID_AAC, CODEC_ID_ADPCM_IMA_QT, CODEC_ID_ADPCM_IMA_WAV, CODEC_ID_ALAC, CODEC_ID_MP3,
    CODEC_ID_PCM_S16LE,
};
use symphonia::core::codecs::audio::{AudioCodecParameters, AudioDecoder};
use symphonia::core::io::MediaSourceStream;
use symphonia::core::packet::Packet;
use symphonia::core::units::{Duration, Timestamp};

/// Encoded MPEG Layer III frames, without container tags. Keep packet boundaries
/// intact: exposing decoded PCM here changes how clients size their buffers.
pub struct Mp3File {
    pub bytes: Vec<u8>,
    pub packets: Vec<std::ops::Range<usize>>,
    pub sample_rate: u32,
    pub channels: u32,
    pub frames_per_packet: u32,
}

pub fn read_mp3(bytes: &[u8]) -> Result<Mp3File, ()> {
    let mss = MediaSourceStream::new(Box::new(Cursor::new(bytes)), Default::default());
    let mut reader = symphonia::default::get_probe()
        .probe(
            &Default::default(),
            mss,
            Default::default(),
            Default::default(),
        )
        .map_err(|_| ())?;
    let track = reader
        .tracks()
        .iter()
        .find(|track| {
            track
                .codec_params
                .as_ref()
                .and_then(|p| p.audio())
                .is_some_and(|p| p.codec == CODEC_ID_MP3)
        })
        .ok_or(())?;
    let params = track.codec_params.as_ref().unwrap().audio().unwrap();
    let mut file = Mp3File {
        bytes: Vec::new(),
        packets: Vec::new(),
        sample_rate: params.sample_rate.ok_or(())?,
        channels: params.channels.as_ref().ok_or(())?.count() as u32,
        frames_per_packet: 0,
    };
    let track_id = track.id;
    while let Some(packet) = reader.next_packet().map_err(|_| ())? {
        if packet.track_id != track_id {
            continue;
        }
        // MPEG-1 Layer III has 1152 samples/frame; MPEG-2 and 2.5 have 576.
        let version = (packet.data.get(1).ok_or(())? >> 3) & 3;
        let frames = if version == 3 { 1152 } else { 576 };
        if file.frames_per_packet != 0 && file.frames_per_packet != frames {
            return Err(());
        }
        file.frames_per_packet = frames;
        let start = file.bytes.len();
        file.bytes.extend_from_slice(&packet.data);
        file.packets.push(start..file.bytes.len());
    }
    if file.packets.is_empty() {
        return Err(());
    }
    Ok(file)
}

/// One decoder per queue preserves the MP3 bit reservoir across buffer boundaries.
pub struct Mp3Decoder(Box<dyn AudioDecoder>);
impl Mp3Decoder {
    pub fn new() -> Result<Self, ()> {
        let mut params = AudioCodecParameters::new();
        params.for_codec(CODEC_ID_MP3);
        symphonia::default::get_codecs()
            .make_audio_decoder(&params, &Default::default())
            .map(Self)
            .map_err(|_| ())
    }
    pub fn reset(&mut self) {
        self.0.reset();
    }
    pub fn decode<'a>(
        &mut self,
        packets: impl IntoIterator<Item = &'a [u8]>,
    ) -> Result<SymphoniaDecodedToPcm, ()> {
        let mut output = SymphoniaDecodedToPcm {
            bytes: Vec::new(),
            sample_rate: 0,
            channels: 0,
        };
        for data in packets {
            let packet = Packet::new(0, Timestamp::ZERO, Duration::ZERO, data);
            let decoded = self.0.decode(&packet).map_err(|_| ())?;
            let rate = decoded.spec().rate();
            let channels = decoded.spec().channels().count() as u32;
            if output.sample_rate != 0
                && (rate != output.sample_rate || channels != output.channels)
            {
                return Err(());
            }
            output.sample_rate = rate;
            output.channels = channels;
            let mut pcm = Vec::new();
            decoded.copy_bytes_to_vec_interleaved_as::<i16>(&mut pcm);
            output.bytes.extend_from_slice(&pcm);
        }
        Ok(output)
    }
}

/// PCM data decoded from an miscellaneous format file.
pub struct SymphoniaDecodedToPcm {
    /// 16-bit little-endian PCM samples, grouped in frames (one sample per
    /// channel in each frame).
    pub bytes: Vec<u8>,
    /// Sample rate in Hz.
    pub sample_rate: u32,
    /// Channel count.
    pub channels: u32,
}

pub fn decode_symphonia_to_pcm(file: Cursor<Vec<u8>>) -> Result<SymphoniaDecodedToPcm, ()> {
    let mss = MediaSourceStream::new(Box::new(file), Default::default());

    // If this failed, the container format is not supported.
    let mut probed = symphonia::default::get_probe()
        .probe(
            &Default::default(),
            mss,
            Default::default(),
            Default::default(),
        )
        .map_err(|_| ())?;

    let track = probed
        .tracks()
        .iter()
        .find(|t| {
            if let Some(codec_params) = &t.codec_params {
                if let Some(audio_codec_params) = codec_params.audio() {
                    audio_codec_params.codec == CODEC_ID_AAC
                        || audio_codec_params.codec == CODEC_ID_ADPCM_IMA_WAV
                        || audio_codec_params.codec == CODEC_ID_ADPCM_IMA_QT
                        || audio_codec_params.codec == CODEC_ID_ALAC
                        || audio_codec_params.codec == CODEC_ID_MP3
                        || audio_codec_params.codec == CODEC_ID_PCM_S16LE
                } else {
                    false
                }
            } else {
                false
            }
        })
        .ok_or(())?;
    let track_id = track.id;

    // Not sure why this would fail, maybe an unusual AAC track.
    let audio_codec_params = track.codec_params.as_ref().unwrap().audio().unwrap();
    let mut decoder = symphonia::default::get_codecs()
        .make_audio_decoder(audio_codec_params, &Default::default())
        .map_err(|_| ())?;

    let mut out_pcm = Vec::<u8>::new();
    let mut audio_spec: Option<AudioSpec> = None;
    {
        let mut tmp_raw_s16_buf: Option<Vec<u8>> = None;
        loop {
            let packet = match probed.next_packet() {
                Ok(packet) => match packet {
                    Some(packet) => packet,
                    // "If Ok(None) is returned, the media has ended and
                    // no more packets will be produced until the reader
                    // is seeked to a new position."
                    None => break,
                },
                // Assume I/O errors can only mean end-of-file, because the
                // entire file is in-memory.
                Err(symphonia::core::errors::Error::IoError(_)) => break,
                Err(_) => return Err(()),
            };

            if packet.track_id != track_id {
                continue;
            }
            let Ok(decoded_packet) = decoder.decode(&packet) else {
                break;
            };

            // For some reason, the "audio spec" (number of channels etc)
            // is reported per-packet? This is weird because it must be the same
            // for all of them.
            let audio_spec = audio_spec.get_or_insert_with(|| decoded_packet.spec().clone());
            assert_eq!(audio_spec, decoded_packet.spec());

            // Note that this assumes every packet's buffer's capacity is the
            // same, which is a dubious assumption, but Symphonia's own example
            // code does it, so maybe it's fine?
            let tmp_raw_s16_buf = tmp_raw_s16_buf
                .get_or_insert_with(|| Vec::with_capacity(decoded_packet.capacity()));
            tmp_raw_s16_buf.clear();
            decoded_packet.copy_bytes_to_vec_interleaved_as::<i16>(tmp_raw_s16_buf);

            out_pcm.extend_from_slice(tmp_raw_s16_buf);
        }
    }
    let audio_spec = audio_spec.ok_or(())?;
    Ok(SymphoniaDecodedToPcm {
        bytes: out_pcm,
        sample_rate: audio_spec.rate(),
        channels: audio_spec.channels().count().try_into().unwrap(),
    })
}
