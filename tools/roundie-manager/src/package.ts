export const MAX_PACKAGE = 224 + 65536;
export const ID_PATTERN = /^[a-z][a-z0-9]*(?:\.[a-z][a-z0-9]*)+$(?![\s\S])/;
export const DIGEST_PATTERN = /^[0-9a-f]{64}$(?![\s\S])/;
export function validId(value: string): boolean { return value.length >= 3 && value.length < 64 && ID_PATTERN.test(value); }
export function validVersion(value: string): boolean {
  return /^(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})$/.test(value) && value.split('.').every(v => Number(v) <= 65535);
}
export function compareVersion(left: string, right: string): number {
  const a = left.split('.').map(Number);
  const b = right.split('.').map(Number);
  for (let i = 0; i < 3; i++) { if (a[i] !== b[i]) { return a[i] - b[i]; } }
  return 0;
}
export function hex(bytes: Uint8Array): string { return Array.from(bytes, v => v.toString(16).padStart(2, '0')).join(''); }
export async function sha256(bytes: Uint8Array): Promise<string> {
  return hex(new Uint8Array(await crypto.subtle.digest('SHA-256', Uint8Array.from(bytes))));
}
export interface PackageMetadata {
  id: string; name: string; version: string; abi: number; target: string; api: number;
  elfSize: number; elfDigest: string; size: number;
}
export interface InspectedPackage extends PackageMetadata {
  bytes: Uint8Array; digest: string; hashMs: number; filename: string;
}

export function inspectMetadata(bytes: Uint8Array): PackageMetadata {
  const fail = (): never => { throw new Error('Invalid or unsupported RPK1 header / metadata.'); };
  if (bytes.length < 276 || bytes.length > MAX_PACKAGE) { fail(); }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const zero = (start: number, end: number) => bytes.subarray(start, end).every(v => v === 0);
  const text = (start: number, length: number): string => {
    const field = bytes.subarray(start, start + length);
    const end = field.indexOf(0);
    if (end <= 0 || !field.subarray(end).every(v => v === 0)) { return fail(); }
    let value: string;
    try { value = new TextDecoder('utf-8', { fatal: true, ignoreBOM: true }).decode(field.subarray(0, end)); }
    catch { return fail(); }
    if (/[\u0000-\u001f\u007f-\u009f]/u.test(value)) { return fail(); }
    return value;
  };
  if (new TextDecoder().decode(bytes.subarray(0, 4)) !== 'RPK1' || view.getUint16(4, true) !== 1 ||
      view.getUint16(6, true) !== 64 || view.getUint32(8, true) !== bytes.length ||
      view.getUint32(12, true) !== 160 || view.getUint32(16, true) !== bytes.length - 224 ||
      !zero(20, 24) || !zero(56, 64) || !zero(218, 224)) { fail(); }
  const id = text(64, 64);
  const name = text(128, 64);
  const target = text(200, 16);
  const abi = view.getUint16(198, true);
  const api = view.getUint16(216, true);
  if (!validId(id) || abi < 1 || abi > 3 || api !== 1 || target !== 'esp32s3.rep1') { fail(); }
  return { id, name, target, abi, api, version: [192, 194, 196].map(at => view.getUint16(at, true)).join('.'),
    elfSize: bytes.length - 224, elfDigest: hex(bytes.subarray(24, 56)), size: bytes.length };
}

export async function inspectFile(file: File): Promise<InspectedPackage> {
  // Check size before allocating from untrusted File metadata.
  if (file.size < 276 || file.size > MAX_PACKAGE || !file.name.toLowerCase().endsWith('.rpk')) {
    throw new Error(`Choose an .rpk file of 276–${MAX_PACKAGE} bytes.`);
  }
  const bytes = new Uint8Array(await file.arrayBuffer());
  const metadata = inspectMetadata(bytes);
  const started = performance.now();
  const digest = await sha256(bytes);
  if (await sha256(bytes.subarray(224)) !== metadata.elfDigest) { throw new Error('Embedded ELF digest mismatch.'); }
  return { ...metadata, bytes, digest, filename: file.name, hashMs: performance.now() - started };
}
