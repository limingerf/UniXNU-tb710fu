/** Verify generated C frames with an unchanged, hash-pinned OneSend receiver. */
import { readFile, writeFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { resolve, dirname } from 'node:path';
import { pathToFileURL, fileURLToPath } from 'node:url';
import { parseArgs } from 'node:util';
import assert from 'node:assert/strict';
const { values } = parseArgs({ options: {
  receiver: { type: 'string' }, frames: { type: 'string' }, output: { type: 'string' },
} });
if (!values.receiver || !values.frames || !values.output) throw Error('Require --receiver, --frames, --output');
const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const manifest = JSON.parse(await readFile(resolve(root, 'diagnostics/early-report/receiver-sources.json'), 'utf8'));
const source = await readFile(values.receiver);
assert.equal(createHash('sha256').update(source).digest('hex'), manifest.files[0].sha256);
const protocol = await import(pathToFileURL(resolve(values.receiver)).href);
const cases = [];
for (const paused of [0, 1]) {
  const expected = await readFile(resolve(values.frames, 'replay-' + paused + '.log'));
  const frames = [];
  for (let i = 0; i < 4; i++) frames.push(await readFile(resolve(values.frames, 'replay-' + paused + '-frame-' + i + '.bin')));
  const headers = frames.map(bytes => protocol.parseFrame(bytes));
  assert.ok(headers.every(Boolean));
  assert.equal(new Set(headers.map(item => item.header.sessionId)).size, 1);
  for (let i = 0; i < 4; i++) {
    assert.equal(headers[i].header.sequence, i);
    assert.equal(Buffer.compare(Buffer.from(protocol.packFrame(headers[i].header, headers[i].block)), frames[i]), 0);
    const receiver = new protocol.OpticalReceiver();
    const event = receiver.consume(frames[i]);
    assert.equal(event?.verified, true, 'Any single variant must finish K=1, including a late arrival');
    const file = await protocol.decodeTransferFile(event.payload);
    assert.equal(file.name, 'TB710FU-live-stage.log');
    assert.equal(Buffer.compare(Buffer.from(file.bytes), expected), 0);
  }
  const receiver = new protocol.OpticalReceiver();
  let deliveries = 0;
  for (let pass = 0; pass < 4; pass++) for (const frame of frames) if (receiver.consume(frame)?.verified) deliveries++;
  assert.equal(deliveries, 1, 'Repeats complete exactly one stable file transfer');
  cases.push({ paused: Boolean(paused), variants: 4, any_one_variant_completes: true,
    exact_file_bytes: expected.length, repeated_frames_deliver_one_file: true });
}
const report = { result: 'PASS', receiver_source: manifest, cases,
  limits: 'Actual receiver/CRC/envelope completion; optical camera acquisition remains a hardware check' };
await writeFile(values.output, JSON.stringify(report, null, 2) + '\n');
console.log(JSON.stringify(report, null, 2));
