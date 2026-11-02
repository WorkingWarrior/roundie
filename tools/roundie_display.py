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
TRACE_V2 = TRACE + ('swap_us driver_us layout_us carry_done last_submit_id last_done_id '
                    'submit_driver_max_us submit_swap_max_us').split()


def request(port, operation='PERF', timeout=4, allow_error=False):
    if operation != 'PERF' and re.fullmatch(r'PERF [0-9]{1,10}|BATCH (?:4|12)|BENCH(?: [0-9]{2,4})?', operation) is None:
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
        if operation.startswith('BENCH'):
            if result['format'] != 3:
                raise ValueError('format')
            result['code'] = int(code)
            if result['code'] and not allow_error:
                raise DeviceError(f'display diagnostic rejected: {result}')
            return result
        if result['format'] not in (1, 2):
            raise ValueError('format')
        trace_names = TRACE if result['format'] == 1 else TRACE_V2
        for field, names in (('summary', SUMMARY), ('trace', trace_names)):
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


def bench(port, frames):
    """Bounded consecutive full frames; the period is GUI iteration end to end."""
    request(port, f'BENCH {frames}')
    deadline = time.monotonic() + frames * 0.2 + 5
    while time.monotonic() < deadline:
        time.sleep(0.5)
        result = request(port, 'BENCH')
        if not result['active']:
            if result['frames'] != frames:
                raise DeviceError(f'benchmark incomplete: {result}')
            result['period_mean_us'] = result['period_sum_us'] / result['frames']
            result['handler_mean_us'] = result['handler_sum_us'] / result['frames']
            result['fps'] = 1000000 / result['period_mean_us']
            return result
    raise TransportError('benchmark deadline exceeded')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('status')
    trace = sub.add_parser('trace')
    trace.add_argument('after', type=int)
    batch = sub.add_parser('batch', help='volatile idle-only comparison; default is 12')
    batch.add_argument('limit', type=int, choices=(4, 12))
    full = sub.add_parser('bench', help='consecutive full-frame period, 30..1000 frames')
    full.add_argument('frames', type=int)
    args = parser.parse_args()
    if args.command == 'bench':
        with open_serial(args.port) as port:
            print(json.dumps(bench(port, args.frames), indent=2))
        return
    operation = 'PERF' if args.command == 'status' else f'PERF {args.after}' if args.command == 'trace' else f'BATCH {args.limit}'
    with open_serial(args.port) as port:
        print(json.dumps(request(port, operation), indent=2))


if __name__ == '__main__':
    main()
