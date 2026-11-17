/* SPDX-License-Identifier: MIT */
import assert from 'node:assert/strict';
import { readFile, writeFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { resolve, dirname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { parseArgs } from 'node:util';

const { values } = parseArgs({ options: {
  receiver: { type: 'string' }, fixtures: { type: 'string' }, output: { type: 'string' },
} });
assert.ok(values.receiver && values.fixtures && values.output);
const root = resolve(dirname(fileURLToPath(import.meta.url)), '../../..');
const manifest = JSON.parse(await readFile(resolve(root, 'diagnostics/early-report/receiver-sources.json'), 'utf8'));
const source = await readFile(values.receiver);
assert.equal(createHash('sha256').update(source).digest('hex'), manifest.files[0].sha256);
const protocol = await import(pathToFileURL(resolve(values.receiver)).href);
const fixtures = JSON.parse(await readFile(resolve(values.fixtures, 'cases.json'), 'utf8'));
const results = [];
const sourceSessions = new Map();
const earlyBackup = await readFile(resolve(values.fixtures, 'early-backup.expected'));

for (const item of fixtures.cases) {
  const expected = await readFile(resolve(values.fixtures, `case-${item.id}.expected`));
  const envelope = await readFile(resolve(values.fixtures, `case-${item.id}.envelope`));
  const bytes = await readFile(resolve(values.fixtures, `case-${item.id}.frames`));
  assert.equal(expected.length, item.bytes);
  assert.equal(BigInt(item.end) - BigInt(item.start), BigInt(item.source_bytes));
  assert.ok(item.source_bytes <= item.bytes);
  assert.equal(item.filename, `TB710FU-${item.source}-${item.generation === '0' ? 'fixed' : 'live'}-${item.generation}-${item.start}-${item.end}.log`);
  if (item.file_body) {
    assert.deepEqual(expected.subarray(item.backup_offset, item.backup_offset + item.backup_bytes), earlyBackup);
    assert.equal(item.bytes - item.backup_offset - item.backup_bytes, item.source_bytes);
  } else assert.equal(item.bytes, item.source_bytes);
  const independentlyEncoded = protocol.encodeTransferFile({ name: item.filename, mimeType: 'text/plain', bytes: expected });
  assert.deepEqual(envelope, Buffer.from(independentlyEncoded));
  assert.equal(bytes.length, item.frames * 752);
  const frames = Array.from({ length: item.frames }, (_, index) => bytes.subarray(index * 752, (index + 1) * 752));
  const parsed = frames.map(frame => protocol.parseFrame(frame));
  assert.ok(parsed.every(Boolean));
  const header = parsed[0].header;
  if (item.id === 0 || item.id === 4) sourceSessions.set(item.source, header.sessionId);
  assert.equal(header.blockCount, item.block_count);
  assert.equal(header.blockLength, 720);
  assert.equal(header.profileId, 0);
  assert.equal(header.payloadChecksum, protocol.crc32(envelope));
  const reference = new protocol.OpticalSender(independentlyEncoded, 'reliable', header.sessionId);
  for (let index = 0; index < frames.length; ++index) {
    const parsedFrame = parsed[index];
    assert.notEqual(parsedFrame.header.sequence % 5, 4);
    assert.equal(parsedFrame.header.sessionId, header.sessionId);
    assert.equal(parsedFrame.header.totalLength, envelope.length);
    assert.deepEqual(Buffer.from(protocol.packFrame(parsedFrame.header, parsedFrame.block)), frames[index]);
    let referenceFrame;
    do { referenceFrame = reference.nextFrame(); } while (referenceFrame.sequence % 5 === 4);
    assert.deepEqual(frames[index], Buffer.from(referenceFrame.bytes));
  }

  async function receive(sequence, description) {
    const receiver = new protocol.OpticalReceiver();
    let deliveries = 0;
    for (const frame of sequence) {
      const event = receiver.consume(frame);
      assert.ok(!event?.error, description);
      if (!event?.verified) continue;
      ++deliveries;
      assert.deepEqual(Buffer.from(event.payload), envelope);
      const file = await protocol.decodeTransferFile(event.payload);
      assert.equal(file.name, item.filename);
      assert.equal(file.mimeType, 'text/plain');
      assert.deepEqual(Buffer.from(file.bytes), expected);
    }
    assert.equal(deliveries, 1, description);
    return receiver;
  }
  const k = item.block_count;
  const completed = await receive(frames, 'four ordered rounds deliver once');
  await receive([...frames.slice(0, k)].reverse().flatMap(frame => [frame, frame]), 'unordered duplicate source blocks');
  const missing = Math.floor(k / 2);
  const incomplete = new protocol.OpticalReceiver();
  for (let index = 0; index < k; ++index) if (index !== missing)
    assert.equal(incomplete.consume(frames[index])?.verified, undefined);
  await receive([...frames.slice(0, k).filter((_, index) => index !== missing), ...frames.slice(k, 2 * k)], 'dropped block recovered on next round');
  const late = Math.floor(k / 2) + 1;
  await receive(frames.slice(late, late + k), 'late join across source rounds');
  const invalid = Buffer.from(frames[0]); invalid[30] ^= 0x80;
  assert.equal(protocol.parseFrame(invalid), null);
  const update = await readFile(resolve(values.fixtures, `case-${item.id}.updated-frames`));
  const updatedExpected = await readFile(resolve(values.fixtures, `case-${item.id}.updated-expected`));
  const updatedFrames = Array.from({ length: update.length / 752 }, (_, index) => update.subarray(index * 752, (index + 1) * 752));
  assert.notEqual(protocol.parseFrame(updatedFrames[0]).header.sessionId, header.sessionId);
  for (const frame of updatedFrames) assert.equal(completed.consume(frame), null, 'changed session requires reset');
  completed.reset();
  let updatedDeliveries = 0;
  for (const frame of updatedFrames) {
    const event = completed.consume(frame);
    if (!event?.verified) continue;
    const file = await protocol.decodeTransferFile(event.payload);
    assert.equal(file.name, `TB710FU-${item.source}-fixed-0-${item.start}-${item.end}.log`);
    assert.deepEqual(Buffer.from(file.bytes), updatedExpected);
    if (item.file_body)
      assert.deepEqual(updatedExpected.subarray(item.backup_offset, item.backup_offset + item.backup_bytes), earlyBackup);
    ++updatedDeliveries;
  }
  assert.equal(updatedDeliveries, 1);
  results.push({ block_count: k, log_bytes: item.bytes, source: item.source,
    source_bytes: item.source_bytes, backup_bytes_retained: item.backup_bytes, rounds: 4,
    fixed_recovery_single_frame: item.generation === '0' ? k === 1 && envelope.length <= 720 : null,
    exact_reference_sender_frames: true, immutable_raw_log_and_all_crcs: true,
    ordered_unordered_duplicate_drop_late_join: 'PASS', corrupted_frame_refused: true,
    replacement_session_requires_receiver_reset: true });
}
assert.deepEqual(results.map(item => item.block_count), [1, 2, 6, 1, 1, 6, 2]);
assert.equal(sourceSessions.size, 2);
assert.notEqual(sourceSessions.get('xnu'), sourceSessions.get('ramoops'), 'source identity isolates sessions even with identical raw content/range/generation');
for (const source of [0, 1]) {
  const expected = await readFile(resolve(values.fixtures, `single-${source}.expected`));
  const envelope = await readFile(resolve(values.fixtures, `single-${source}.envelope`));
  const frame = await readFile(resolve(values.fixtures, `single-${source}.frame`));
  const parsed = protocol.parseFrame(frame);
  assert.ok(parsed);
  assert.equal(parsed.header.blockCount, 1);
  assert.equal(parsed.header.totalLength, 720);
  const reference = new protocol.OpticalSender(envelope, 'reliable', parsed.header.sessionId);
  assert.deepEqual(Buffer.from(reference.nextFrame().bytes), frame);
  const receiver = new protocol.OpticalReceiver();
  const event = receiver.consume(frame);
  assert.equal(event?.verified, true, 'one still frame completes the transfer');
  assert.deepEqual(Buffer.from(event.payload), envelope);
  const decoded = await protocol.decodeTransferFile(event.payload);
  assert.deepEqual(Buffer.from(decoded.bytes), expected);
  assert.match(decoded.name, source ? /^TB710FU-xnu-fixed-/ : /^TB710FU-ramoops-fixed-/);
}
const result = { result: 'PASS', receiver_source: manifest, cases: results,
  single_frame_capacity_boundary_and_still_frame_recovery: 'PASS',
  limits: 'Portable C sender and unchanged pinned browser protocol; camera acquisition, kernel scheduling and real ramoops bytes remain hardware gates.' };
await writeFile(values.output, JSON.stringify(result, null, 2) + '\n');
console.log(JSON.stringify(result, null, 2));
