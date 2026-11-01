// Shared by both players. packet: [flags u8][h264 annex b]
export const FRAME_KEY = 1

export function parsePacket(packet) {
  const bytes = new Uint8Array(packet)
  return { keyframe: (bytes[0] & FRAME_KEY) != 0, data: bytes.subarray(1) }
}

// sps: nal without the start code
export function avcCodec(sps) {
  const hex = v => v.toString(16).padStart(2, '0')
  return `avc1.${hex(sps[1])}${hex(sps[2])}${hex(sps[3])}`
}

// hardware when the browser has it, otherwise whatever it can do
export async function supportedConfig(Codec, config) {
  for (const candidate of [{ ...config, hardwareAcceleration: 'prefer-hardware' }, config]) {
    if ((await Codec.isConfigSupported(candidate).catch(() => null))?.supported) {
      return candidate
    }
  }
  return null
}
