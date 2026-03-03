#!/usr/bin/env python3
import re
import sys
from pathlib import Path

SKIP_PREFIXES = (
    '#', 'using ', 'typedef ', 'template', 'inline ', 'struct ', 'class ',
    'enum ', 'namespace ', 'extern ', 'static_assert', 'constexpr '
)


def split_top_level_statements(text: str):
    # Walk the file once and only emit semicolon-terminated statements that
    # appear at depth 0. This avoids pulling declarations out of function bodies.
    spans = []
    start = None
    depth = 0
    i = 0
    n = len(text)
    in_line_comment = False
    in_block_comment = False
    in_string = False
    string_char = ''

    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ''

        if in_line_comment:
            if ch == '\n':
                in_line_comment = False
            i += 1
            continue
        if in_block_comment:
            if ch == '*' and nxt == '/':
                in_block_comment = False
                i += 2
            else:
                i += 1
            continue
        if in_string:
            if ch == '\\':
                i += 2
                continue
            if ch == string_char:
                in_string = False
            i += 1
            continue

        if ch == '/' and nxt == '/':
            in_line_comment = True
            i += 2
            continue
        if ch == '/' and nxt == '*':
            in_block_comment = True
            i += 2
            continue
        if ch in ('"', "'"):
            in_string = True
            string_char = ch
            i += 1
            continue

        if ch == '{':
            depth += 1
        elif ch == '}':
            depth = max(depth - 1, 0)
        elif depth == 0:
            if start is None and not ch.isspace():
                start = i
            if ch == ';' and start is not None:
                spans.append((start, i + 1, text[start:i + 1]))
                start = None
        i += 1
    return spans


def is_variable_statement(stmt: str) -> bool:
    # We only want file-scope storage declarations here. Everything else stays
    # in the generated kernel body unchanged.
    stripped = stmt.strip()
    if not stripped:
        return False
    if stripped.startswith(SKIP_PREFIXES):
        return False
    if '__attribute__' in stripped and '(' in stripped:
        return False
    if '(' in stripped and ')' in stripped:
        return False
    return True


def extract_var_name(stmt: str) -> str:
    body = stmt.strip().rstrip(';').strip()
    match = re.search(r'([A-Za-z_][A-Za-z0-9_]*)\s*(\[[^\]]*\])?\s*(?:=\s*.*)?$', body, re.S)
    if not match:
        raise RuntimeError(f'failed to extract variable name from: {stmt}')
    return match.group(1)


def remove_spans(text: str, spans):
    # Remove the extracted declarations from the kernel source so we can
    # re-home them inside the generated scratchpad struct.
    out = []
    cursor = 0
    for start, end, _ in spans:
        out.append(text[cursor:start])
        out.append('\n')
        cursor = end
    out.append(text[cursor:])
    return ''.join(out)


def split_params(param_text: str):
    text = param_text.strip()
    if not text or text == 'void':
        return []
    params = []
    current = []
    depth = 0
    for ch in text:
        if ch in '(<[':
            depth += 1
        elif ch in ')>]':
            depth -= 1
        if ch == ',' and depth == 0:
            params.append(''.join(current).strip())
            current = []
            continue
        current.append(ch)
    if current:
        params.append(''.join(current).strip())
    return params


def extract_param_type(param: str) -> str:
    body = re.sub(r'=.*$', '', param).strip()
    match = re.match(r'(.+\b)([A-Za-z_][A-Za-z0-9_]*)\s*$', body, re.S)
    if match:
        return match.group(1).strip()
    return body


def wrapper_arg(param_type: str, index: int) -> str:
    # Kernel pointer arguments are EVAs on the host side and native pointers in
    # the generated kernel entry point, so pointers need translation here.
    if '*' in param_type:
        return f'reinterpret_cast<{param_type}>(hb_native_sim::eva_to_ptr(argv[{index}]))'
    return f'static_cast<{param_type}>(argv[{index}])'


def main() -> int:
    if len(sys.argv) != 3:
        print('usage: generate_kernel.py <input> <output>', file=sys.stderr)
        return 1

    src_path = Path(sys.argv[1])
    out_path = Path(sys.argv[2])
    original = src_path.read_text()

    # Native wait/synchronization uses a richer type than the device source.
    # These rewrites are intentionally narrow: only the lr/lr.aq wait-word path.
    transformed = original.replace('volatile int', 'hb_native_sim::wait_word')
    transformed = re.sub(r'bsg_lr\s*\(\s*\(int\s*\*\)\s*&', 'bsg_lr(&', transformed)
    transformed = re.sub(r'bsg_lr_aq\s*\(\s*\(int\s*\*\)\s*&', 'bsg_lr_aq(&', transformed)
    transformed = re.sub(r'\(hb_native_sim::wait_word \*\)', '(hb_native_sim::wait_word *)', transformed)

    statements = split_top_level_statements(transformed)
    var_spans = [(start, end, stmt) for start, end, stmt in statements if is_variable_statement(stmt)]
    var_names = [extract_var_name(stmt) for _, _, stmt in var_spans]
    var_decls = [stmt.strip() for _, _, stmt in var_spans]

    if var_spans:
        first_var_start = var_spans[0][0]
        prefix = transformed[:first_var_start]
    else:
        first_var_start = len(transformed)
        prefix = transformed
    body = remove_spans(transformed[first_var_start:], [(start - first_var_start, end - first_var_start, stmt) for start, end, stmt in var_spans])

    kernels = re.findall(r'extern\s+"C"\s+int\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(([^)]*)\)', transformed)
    if not kernels:
        raise RuntimeError('no extern "C" kernel functions found')

    lines = []
    lines.append('#include "runtime.hpp"')
    lines.append('#include <cstddef>')
    lines.append('#include <cstdint>')
    lines.append('#include <new>')
    lines.append('')
    lines.append(prefix.rstrip())
    lines.append('')
    # Every file-scope variable declaration from the device kernel becomes a
    # field in this struct, giving each simulated core its own scratchpad copy.
    lines.append('namespace hb_native_generated {')
    lines.append('struct scratchpad_t {')
    for decl in var_decls:
        lines.append(f'  {decl}')
    lines.append('};')
    lines.append('inline scratchpad_t& scratchpad() {')
    lines.append('  return *reinterpret_cast<scratchpad_t*>(hb_native_sim::current_scratchpad_base());')
    lines.append('}')
    lines.append('}  // namespace hb_native_generated')
    lines.append('')
    # Rebind the original global names so the rest of kernel.cpp can compile
    # without source-level scratchpad annotations.
    for name in var_names:
        lines.append(f'#define {name} (hb_native_generated::scratchpad().{name})')
    lines.append('')
    lines.append(body.lstrip())
    lines.append('')
    lines.append('namespace {')
    # Construct one scratchpad_t per simulated tile before launching threads.
    lines.append('void hb_native_construct_scratchpads(void* base, std::size_t count) {')
    lines.append('  auto* scratchpads = static_cast<hb_native_generated::scratchpad_t*>(base);')
    lines.append('  for (std::size_t i = 0; i < count; ++i) {')
    lines.append('    new (&scratchpads[i]) hb_native_generated::scratchpad_t();')
    lines.append('  }')
    lines.append('}')
    lines.append('void hb_native_destroy_scratchpads(void* base, std::size_t count) {')
    lines.append('  auto* scratchpads = static_cast<hb_native_generated::scratchpad_t*>(base);')
    lines.append('  for (std::size_t i = 0; i < count; ++i) {')
    lines.append('    scratchpads[i].~scratchpad_t();')
    lines.append('  }')
    lines.append('}')
    for name, params in kernels:
        param_types = [extract_param_type(param) for param in split_params(params)]
        args = ', '.join(wrapper_arg(param_type, idx) for idx, param_type in enumerate(param_types))
        # Register a native entry point that decodes CUDA-style argv and calls
        # the original kernel symbol directly.
        lines.append(f'int hb_native_invoke_{name}(const std::uint32_t* argv) {{')
        lines.append(f'  return {name}({args});')
        lines.append('}')
        lines.append(f'const bool hb_native_registered_{name} = []() {{')
        lines.append('  hb_native_sim::register_kernel({')
        lines.append(f'      "{name}",')
        lines.append(f'      hb_native_invoke_{name},')
        lines.append('      sizeof(hb_native_generated::scratchpad_t),')
        lines.append('      alignof(hb_native_generated::scratchpad_t),')
        lines.append('      hb_native_construct_scratchpads,')
        lines.append('      hb_native_destroy_scratchpads,')
        lines.append('  });')
        lines.append('  return true;')
        lines.append('}();')
    lines.append('}  // namespace')
    lines.append('')

    out_path.write_text('\n'.join(lines))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
