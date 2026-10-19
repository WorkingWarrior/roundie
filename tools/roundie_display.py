#!/usr/bin/env python3
"""Bounded GUI/LCD counters and 32 retained active-iteration traces (RP1)."""
import argparse
import json
import re
import secrets
import time
from roundie_time import DeviceError, TransportError, open_serial, read_serial_reply, write_command

SUMMARY = ('batch loops flushes pixels errors handler_max_us pump_max_us wake_max_us '
           'dma_max_us submit_max_us wait_max_us stack_min sequence dma_completions').split()
TRACE = ('sequence at_us nav request generation batch pending_before pending_after applied '
         'pump_us handler_us wake_us flushes done pixels submit_us dma_us wait_us render_us '
         'invalidations invalid_pixels full_invalidations').split()


def request(port, operation='PERF', timeout=4, allow_error=False):
    if operation != 'PERF' and re.fullmatch(r'PERF [0-9]{1,10}|BATCH (?:4|12)', operation) is None:
        raise DeviceError('invalid display diagnostic')
    if operation.startswith('PERF ') and int(operation[5:]) > 0xffffffff:
        raise DeviceError('cursor exceeds uint32')
    identity = secrets.token_hex(4)
    prefix = f'!RP1 {identity} '.encode()
    write_command(port, f'!RP1 {identity} {operation}')
    line = read_serial_reply(port, prefix, time.monotonic() + timeout, 1400)
    try:
        code, payload = line[len(prefix):].split(b' ', 1)
        if re.fullmatch(rb'-?[0-9]{1,10}', code) is None:
            raise ValueError('code')
        result = json.loads(payload)
        if result['format'] != 1:
            raise ValueError('format')
        for field, names in (('summary', SUMMARY), ('trace', TRACE)):
            values = result[field]
            if (not isinstance(values, list) or len(values) != len(names) or
                    any(type(v) is not int or not 0 <= v < 2 ** 64 for v in values)):
                raise ValueError(field)
            result[field] = dict(zip(names, values))
        if result['summary']['batch'] not in (4, 12):
            raise ValueError('batch')
        if any(type(result[k]) is not int or not 0 <= result[k] <= 0xffffffff for k in ('oldest', 'latest')):
            raise ValueError('cursor')
        result['code'] = int(code)
    except (ValueError, KeyError, TypeError) as error:
        raise TransportError(f'malformed display diagnostics: {error}') from error
    if result['code'] and not allow_error:
        raise DeviceError(f'display diagnostic rejected: {result}')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('status')
    trace = sub.add_parser('trace')
    trace.add_argument('after', type=int)
    batch = sub.add_parser('batch', help='volatile idle-only comparison; default is 12')
    batch.add_argument('limit', type=int, choices=(4, 12))
    args = parser.parse_args()
    operation = 'PERF' if args.command == 'status' else f'PERF {args.after}' if args.command == 'trace' else f'BATCH {args.limit}'
    with open_serial(args.port) as port:
        print(json.dumps(request(port, operation), indent=2))


if __name__ == '__main__':
    main()
