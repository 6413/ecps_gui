// Checks the protocol code without a server: run with `npm run selftest`.
import assert from 'node:assert/strict'
import crypto from 'node:crypto'
import { deriveKeys, open, parseStreamHeader, seal, writeStreamHeader } from './ecps.js'

// RFC 8439 section 2.8.2 AEAD test vector, nonce rebuilt from the ecps layout
{
  const key = Buffer.from('808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f', 'hex')
  const aad = Buffer.from('50515253c0c1c2c3c4c5c6c7', 'hex')
  const plain = Buffer.from('Ladies and Gentlemen of the class of \'99: If I could offer you only one tip for the future, sunscreen would be it.')
  const nonce = Buffer.from('070000004041424344454647', 'hex')
  const c = crypto.createCipheriv('chacha20-poly1305', key, nonce, { authTagLength: 16 })
  c.setAAD(aad, { plaintextLength: plain.length })
  const body = c.update(plain)
  c.final()
  assert.equal(c.getAuthTag().toString('hex'), '1ae10b594f09e26a7e902ecbd0600691')
  assert.equal(body.subarray(0, 4).toString('hex'), 'd31a8d34')
}

// seal and open must agree, and tampering must fail
{
  const key = crypto.randomBytes(32)
  const aad = Buffer.from([1, 2, 3])
  const sealed = seal(key, 7n, aad, Buffer.from('hello'))
  assert.equal(open(key, 7n, aad, sealed).toString(), 'hello')
  assert.equal(open(key, 8n, aad, sealed), null)
  sealed[0] ^= 1
  assert.equal(open(key, 7n, aad, sealed), null)
}

// both ends of the handshake must derive the same keys
{
  const client = crypto.generateKeyPairSync('x25519')
  const ephemeral = crypto.generateKeyPairSync('x25519')
  const fixed = crypto.generateKeyPairSync('x25519')
  const raw = k => Buffer.from(k.export({ format: 'jwk' }).x, 'base64url')
  const keys = deriveKeys(client.privateKey, raw(client.publicKey), raw(ephemeral.publicKey), raw(fixed.publicKey))
  const dh = Buffer.concat([
    crypto.diffieHellman({ privateKey: ephemeral.privateKey, publicKey: client.publicKey }),
    crypto.diffieHellman({ privateKey: fixed.privateKey, publicKey: client.publicKey })
  ])
  const salt = Buffer.concat([raw(client.publicKey), raw(ephemeral.publicKey), raw(fixed.publicKey)])
  const server = Buffer.from(crypto.hkdfSync('sha256', dh, salt, 'ecps udp v1', 144))
  assert.deepEqual(Buffer.from(keys.c2s), server.subarray(0, 32))
  assert.deepEqual(Buffer.from(keys.tcpS2c), server.subarray(112, 144))
}

// stream headers against values packed the way prot.h packs them
{
  for (const [sequence, current, possible, flag] of [[0, 0, 1, 0], [0xabc, 0, 0x123, 1], [0xfff, 0xfff, 0, 0], [0x801, 0x234, 0, 0]]) {
    const parsed = parseStreamHeader(writeStreamHeader(sequence, current, possible, flag))
    assert.equal(parsed.sequence, sequence)
    assert.equal(parsed.current, current)
    if (current == 0) {
      assert.equal(parsed.possible, possible)
      assert.equal(parsed.flag, flag)
    }
  }
  // SetSequence(0xabc): low byte 0xbc, high nibble in the top of sc[1]
  assert.deepEqual([...writeStreamHeader(0xabc, 0, 1, 0)].slice(0, 3), [0xbc, 0xa0, 0x00])
  // SetCurrent(0x234): low nibble in the bottom of sc[1], the rest in sc[2]
  assert.deepEqual([...writeStreamHeader(0, 0x234, 0, 0)], [0x00, 0x04, 0x23])
}

console.log('selftest ok')
