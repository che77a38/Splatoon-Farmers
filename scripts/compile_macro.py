#!/usr/bin/env python3
"""Compile 6-gen 文字版代码 (.txt) scripts into firmware/.h headers.

Source of truth for the user's script collection lives under scripts/macros/.
This compiler lowers a useful subset of the DSL to the same MacroStep array
format that MaterialFarmMacro.h uses, so the runtime MacroEngine keeps its
MCU-timed execution semantics and the picker UI can stay agnostic to where
the script came from.

DSL subset supported at default settings (defaults are folded at compile time):

- Title + 描述 metadata
- 数值选项: name, default, min, max          (folded to default)
- 列表选项: name, opt1, opt2, ...           (folded to first option)
- $var = expr ; $var += expr                (constant-folded only)
- WAIT ms
- X/Y/A/B/L/R/ZL/ZR [duration]             (button press; auto-release)
- ZL_DOWN, ZL_UP, ZR_DOWN, ZR_UP           (trigger held / released)
- LS_<UP|DOWN|LEFT|RIGHT|UP_LEFT|UP_RIGHT|DOWN_LEFT|DOWN_RIGHT> [duration]
- RS_<dir> [duration]                       (right stick)
- LS_RESET, RS_RESET
- IF cond == cond [ELSE] ENDIF             (constant-fold only)
- FOR ... NEXT                              (emit body once; firmware loops)
- FUNC name / ENDFUNC
- CALL name                                 (inlined; not yet recursive)
- PRINT "..."                                (compile-time no-op)

Skipped, with a printed warning:

- AUDIO_LOAD (no firmware microphone path)
- Recursive CALL
- Reference to an option / function that is not defined

Output:

- firmware/include/Script_<Name>.h         (one per .txt)
- firmware/include/scripts_index.inc        (auto-discovered list of chips)

Usage:

    scripts/compile_macro.py scripts/macros/                    # all
    scripts/compile_macro.py scripts/macros/foo.txt             # one
    scripts/compile_macro.py --src DIR --include DIR --index PATH

Run from the repo root; absolute paths also work.
"""

import argparse
import os
import re
import sys
from dataclasses import dataclass, field
from typing import Optional, List, Tuple, Dict, Any


# --- HID code tables (mirror firmware/include + web/manual-input.js) ---

# Per web/manual-input.js BUTTON_BITS; switch_ESP32 packs face buttons Y B A X
# in low bits. The existing MaterialFarmMacro.h head lists B=bit1, A=bit2,
# X=bit3, L=bit4, R=bit5. Y=bit0 / ZL=bit6 / ZR=bit7 are new for this
# compiler so the user's 天埠罗 + dpad-triggers scripts keep working.
BUTTON_BITS = {
    'Y': 1 << 0,
    'B': 1 << 1,
    'A': 1 << 2,
    'X': 1 << 3,
    'L': 1 << 4,
    'R': 1 << 5,
    'ZL': 1 << 6,
    'ZR': 1 << 7,
    'MINUS': 1 << 8,
    'PLUS': 1 << 9,
    'L_STICK_PRESS': 1 << 10,
    'R_STICK_PRESS': 1 << 11,
    'HOME': 1 << 12,
    'CAPTURE': 1 << 13,
}

DPAD_DIR_BARE = {
    'UP': 0,
    'UP_RIGHT': 1,
    'RIGHT': 2,
    'DOWN_RIGHT': 3,
    'DOWN': 4,
    'DOWN_LEFT': 5,
    'LEFT': 6,
    'UP_LEFT': 7,
}

# Map LS_<dir> to (lx, ly). 0..255 with 128 centered, 0 = full up / left,
# 255 = full down / right. Matches the existing kLeftUp convention.
STICK_DIRS = {
    'UP':         (128, 0),
    'DOWN':       (128, 255),
    'LEFT':       (0, 128),
    'RIGHT':      (255, 128),
    'UP_LEFT':    (0, 0),
    'UP_RIGHT':   (255, 0),
    'DOWN_LEFT':  (0, 255),
    'DOWN_RIGHT': (255, 255),
}

DPAD_CENTERED = 15
AXIS_CENTERED = 128

# --- AST nodes (minimal, the compiler is single-pass) ---


@dataclass
class Step:
    """One MacroStep entry: a held controller state for duration_ms.

    Matches farmers::MacroStep in firmware/include/MacroEngine.h.
    """
    duration_ms: int
    buttons: int = 0
    dpad: int = DPAD_CENTERED
    lx: int = AXIS_CENTERED
    ly: int = AXIS_CENTERED
    rx: int = AXIS_CENTERED
    ry: int = AXIS_CENTERED


# ---- parse ----


@dataclass
class Statement:
    """One source line after tokenisation; holds enough state for the emitter
    to know what to do (and whether a duration is part of the same line)."""
    kind: str  # 'wait', 'press', 'btn_state', 'stick_set', 'stick_reset',
              # 'dpad_set', 'if', 'assign', 'print', 'comment_eaten', 'call'
    raw: str = ''
    button: str = ''
    duration_ms: Optional[int] = None  # present for wait/press/stick_set
    stick: str = ''                    # 'L' or 'R' for stick_set / stick_reset
    direction: str = ''                # 'UP', 'DOWN', etc.
    hold: bool = False                 # for btn_state: True=DOWN, False=UP
    block: List['Statement'] = field(default_factory=list)
    cond_lhs: str = ''
    cond_op: str = ''
    cond_rhs: str = ''
    var: str = ''
    op: str = ''                       # '=' or '+='
    rhs_text: str = ''
    text: str = ''
    name: str = ''                     # for call: function name
    release: bool = False              # for stick_set_state UP-suffix: release direction
    # IF/ELIF/ELSE branches. Each branch is a dict {'cond': (lhs, op, rhs) | None,
    # 'block': [Statement, ...]}. cond=None means an unconditional ELSE.
    branches: List[Dict[str, Any]] = field(default_factory=list)

    def line_repr(self) -> str:
        return self.raw


def strip_inline_comment(s: str) -> str:
    """Drop '#' to end of line, but not inside double-quoted strings."""
    out = []
    in_str = False
    for ch in s:
        if ch == '"':
            in_str = not in_str
            out.append(ch)
        elif ch == '#' and not in_str:
            break
        else:
            out.append(ch)
    return ''.join(out).strip()


def split_top_level(s: str) -> List[str]:
    """Split on whitespace / commas at the top level (no parentheses)."""
    # Simple approach: split on whitespace and commas.
    out = []
    for tok in re.split(r'[,\s]+', s):
        if tok:
            out.append(tok)
    return out


def parse_top_lines(text: str) -> Tuple[Dict[str, Any], List[str]]:
    """Walk the script top section, lifting 标题/描述/数值选项/列表选项 into
    a metadata dict and returning the remainder (body lines) untouched."""
    meta = {
        'title': '',
        'description': '',
        'numeric_options': {},  # name -> (default, min, max)
        'list_options': {},     # name -> [opts...]
    }
    body: List[str] = []
    in_meta = True
    for raw_line in text.splitlines():
        s = strip_inline_comment(raw_line)
        if in_meta:
            if not s:
                continue  # blank line in metadata is fine
            if s.startswith('标题:'):
                meta['title'] = s[len('标题:'):].strip()
                continue
            if s.startswith('描述:'):
                meta['description'] = s[len('描述:'):].strip()
                continue
            if s.startswith('数值选项:'):
                parts = [p.strip() for p in s[len('数值选项:'):].split(',')]
                if len(parts) != 4:
                    raise ValueError(f'数值选项 malformed: {raw_line!r}')
                name, default, mn, mx = (parts[0], int(parts[1]),
                                          int(parts[2]), int(parts[3]))
                meta['numeric_options'][name] = (default, mn, mx)
                continue
            if s.startswith('列表选项:'):
                parts = [p.strip() for p in s[len('列表选项:'):].split(',')]
                name = parts[0]
                meta['list_options'][name] = parts[1:]
                continue
            # Anything else means we've left the metadata header.
            in_meta = False
        body.append(raw_line)
    return meta, body


def parse_body(body_lines: List[str], source_name: str = '<script>') -> List[Statement]:
    """Parse the body into a list of statements, including nested IF/FUNC blocks."""
    out: List[Statement] = []
    i = 0
    indent_stack = [0]  # current indentation level for nested blocks

    # Pass 1: tokenize each non-empty, non-comment line. Track FUNC / IF / FOR
    # blocks and consume until their matching ENDIF / ENDFUNC / NEXT.
    while i < len(body_lines):
        raw = body_lines[i]
        s = strip_inline_comment(raw)
        if not s:
            i += 1
            continue

        # Detect indentation (count leading spaces of raw, not stripped).
        indent = len(raw) - len(raw.lstrip(' '))
        # Trim indent_stack to current level.
        while len(indent_stack) > 1 and indent <= indent_stack[-1]:
            indent_stack.pop()

        first = s.split()[0] if s.split() else ''
        tokens = split_top_level(s)

        if first == 'WAIT':
            # WAIT ms   |   WAIT 进图时间*1000   |   WAIT $var+500
            expr = s[len('WAIT'):].strip()
            out.append(Statement(kind='wait', raw=raw, duration_ms=None,
                                 text=expr))
            i += 1
        elif first in BUTTON_BITS:
            # Two-token form: BUTTON 50 | BUTTON DOWN | BUTTON UP
            if len(tokens) == 2 and tokens[1].lstrip('-').isdigit():
                ms = int(tokens[1])
                out.append(Statement(kind='press', raw=raw,
                                     button=first, duration_ms=ms))
            elif len(tokens) == 2 and tokens[1] in ('DOWN', 'UP'):
                out.append(Statement(kind='btn_state', raw=raw,
                                     button=first,
                                     hold=(tokens[1] == 'DOWN')))
            elif len(tokens) == 1:
                # Bare button — treat as zero-duration state-marker. The
                # firmware cannot emit a frame without duration, so emit a
                # 1 ms sentinel frame and continue. (Rare; mostly the user's
                # scripts always supply either DOWN/UP or a duration.)
                out.append(Statement(kind='press', raw=raw, button=first,
                                     duration_ms=1))
            else:
                raise ValueError(f'{source_name}: bad token after {first!r}: {raw!r}')
            i += 1
        elif first.startswith('LS_') or first.startswith('RS_'):
            stem = first[:2]  # 'LS' or 'RS'
            tail = first[3:]
            if tail == 'RESET':
                out.append(Statement(kind='stick_reset', raw=raw, stick=stem[0]))
                i += 1
                continue
            if tail not in STICK_DIRS:
                raise ValueError(f'{source_name}: bad stick cmd {first!r} at line {raw!r}')
            # Three forms for LS_<DIR>:
            #   LS_UP 1800        — set direction, hold for N ms (auto-reset)
            #   LS_UP DOWN        — set direction, persist (no duration)
            #   LS_UP UP          — release the matching persisted direction
            if len(tokens) >= 2 and tokens[1].isdigit():
                dur = int(tokens[1])
                out.append(Statement(kind='stick_set', raw=raw, stick=stem[0],
                                     direction=tail, duration_ms=dur))
            elif len(tokens) >= 2 and tokens[1] == 'DOWN':
                out.append(Statement(kind='stick_set_state', raw=raw,
                                     stick=stem[0],
                                     direction=tail,
                                     duration_ms=None))
            elif len(tokens) >= 2 and tokens[1] == 'UP':
                out.append(Statement(kind='stick_set_state', raw=raw,
                                     stick=stem[0],
                                     direction=tail,
                                     duration_ms=0,
                                     release=True))
            else:
                raise ValueError(f'{source_name}: bad stick cmd {first!r} at line {raw!r}')
            i += 1
        elif first in DPAD_DIR_BARE and len(tokens) == 2 \
                and tokens[1].lstrip('-').isdigit():
            # Bare cardinal direction with duration: UP 50 / DOWN 50 / ...
            # Dpad direction press held for N ms (auto-release).
            ms = int(tokens[1])
            out.append(Statement(kind='dpad_press', raw=raw,
                                 direction=first, duration_ms=ms))
            i += 1
        elif first == '$':
            # Already handled in the next branch; alias kept for clarity.
            i += 1  # skip stray lines
        elif s.startswith('$'):
            # Assignment: $var = expr   |   $var += expr
            m = re.match(r'(\$\w+)\s*([+\-]?=)\s*(.+)$', s)
            if not m:
                raise ValueError(f'{source_name}: bad assign at {raw!r}')
            var, op, rhs_text = m.group(1), m.group(2), m.group(3).strip()
            out.append(Statement(kind='assign', raw=raw,
                                 var=var, op=op, rhs_text=rhs_text))
            i += 1
        elif first == 'PRINT':
            text = s[len('PRINT'):].strip()
            out.append(Statement(kind='print', raw=raw, text=text))
            i += 1
        elif first == 'IF':
            # IF <lhs> <comp_op> <rhs>. lhs/rhs may contain arithmetic like
            # `$计数器 % ($配件ACD*1000) == 0`, so we can't pin them to \S+
            # — use a non-greedy capture up to the first comparison op.
            m = re.match(r'IF\s+(.+?)\s*(==|!=|>=|<=|>|<)\s*(.+)$', s)
            if not m:
                raise ValueError(f'{source_name}: bad IF at {raw!r}')
            branches, end_idx = _consume_if_chain(
                body_lines, i + 1, source_name)
            # branches[0] is the IF body; fill in its cond from the IF line.
            branches[0]['cond'] = (m.group(1).strip(), m.group(2),
                                   m.group(3).strip())
            out.append(Statement(kind='if_chain', raw=raw,
                                 branches=branches))
            i = end_idx + 1
        elif first == 'FOR':
            # Optional explicit iteration count (FOR 40) defaults to "until
            # firmware STOP". The compiler emits the body once either way;
            # FOR-with-count is unrolled at compile time so the runtime loop
            # step is exact (40× of the body). Constant-fold count only.
            count = None
            if len(tokens) > 1:
                try:
                    count = int(tokens[1])
                except ValueError:
                    count = None
            block, end_idx = _consume_block(body_lines, i + 1, indent, 'FOR')
            if count is not None:
                for _ in range(count):
                    out.extend(block)
            else:
                # Body once; firmware loops with repeat=true.
                out.extend(block)
            # Mark the original FOR for no-op (already expanded).
            i = end_idx + 1
        elif first == 'NEXT':
            raise ValueError(f'{source_name}: stray NEXT at line {raw!r}')
        elif first == 'ENDIF':
            raise ValueError(f'{source_name}: stray ENDIF at line {raw!r}')
        elif first == 'FUNC':
            name = tokens[1]
            block, end_idx = _consume_block(body_lines, i + 1, indent, 'FUNC')
            out.append(Statement(kind='func_def', raw=raw, name=name,
                                 block=block))
            i = end_idx + 1
        elif first == 'ENDFUNC':
            raise ValueError(f'{source_name}: stray ENDFUNC at line {raw!r}')
        elif first == 'CALL':
            out.append(Statement(kind='call', raw=raw, name=tokens[1]))
            i += 1
        elif first == 'AUDIO_LOAD':
            # firmware has no audio path; the calling code must check.
            out.append(Statement(kind='unsupported',
                                 raw=raw,
                                 text='AUDIO_LOAD not supported by firmware'))
            i += 1
        elif first == 'RETURN':
            out.append(Statement(kind='return', raw=raw))
            i += 1
        elif first in ('BREAK', 'CONTINUE'):
            # FOR loops are either constant-counted (unrolled, so BREAK /
            # CONTINUE have no meaning) or unbounded (firmware-side repeat,
            # also a no-op for compile-time). Acknowledge the line and move
            # on so the user-facing script keeps its readable shape.
            sys.stderr.write(
                f'NOTE: {first} at line {raw.strip()!r} skipped (loop unrolled '
                f'or firmware-driven; no per-frame effect)\n')
            i += 1
        else:
            raise ValueError(f'{source_name}: unparsed line {raw!r}')

    return out


def _consume_if_chain(body_lines, start, source_name):
    """Walk forward from `start` until matching ENDIF at depth 0.

    Returns (branches, end_idx) where branches is a list of
    {'cond': (lhs, op, rhs) | None, 'block': [Statement]} entries:

    - branches[0] is the IF body, with cond=None. The caller (the IF-handler
      in parse_body) fills the cond from the IF line's regex match.
    - branches[1:] hold ELIF (cond set) and trailing ELSE (cond=None) clauses
      in source order.

    Indent-free: depth counter tolerates the user's occasional off-by-one
    column spacing (e.g. ELIF at indent 9 instead of 8) without derailing.
    """
    # Build a flat list of (cond_or_None, raw_block_lines) pairs, then turn
    # each block into Statements at the end. Keeping the cond next to its
    # block avoids the "where do I stash the pending ELIF cond" puzzle.
    pairs: List[Tuple[Any, List[str]]] = []
    current_block: List[str] = []
    pending_cond: Any = None
    depth = 0
    i = start
    while i < len(body_lines):
        raw = body_lines[i]
        s = strip_inline_comment(raw)
        if not s:
            current_block.append(raw)
            i += 1
            continue
        first = s.split()[0]
        if first == 'IF':
            depth += 1
            current_block.append(raw)
            i += 1
            continue
        if first == 'ENDIF':
            if depth == 0:
                pairs.append((pending_cond, current_block))
                branches = [{'cond': c, 'block': parse_body(blk, source_name)}
                            for c, blk in pairs]
                return branches, i
            depth -= 1
            current_block.append(raw)
            i += 1
            continue
        if first == 'ELIF' and depth == 0:
            m = re.match(r'ELIF\s+(.+?)\s*(==|!=|>=|<=|>|<)\s*(.+)$', s)
            if not m:
                raise ValueError(f'{source_name}: bad ELIF at {raw!r}')
            pairs.append((pending_cond, current_block))
            pending_cond = (m.group(1).strip(), m.group(2),
                            m.group(3).strip())
            current_block = []
            i += 1
            continue
        if first == 'ELSE' and depth == 0:
            pairs.append((pending_cond, current_block))
            pending_cond = None
            current_block = []
            i += 1
            continue
        current_block.append(raw)
        i += 1
    raise ValueError(f'unterminated IF chain starting at line {start}')


def _consume_block(body_lines, start, parent_indent, kind: str) -> Tuple[List[Statement], int]:
    """Consume lines at greater indentation than parent_indent until the
    matching terminator (ENDIF / NEXT / ENDFUNC) is reached.

    The terminator must sit at parent_indent; if it sits inside the block
    (deeper indent), this is a nested block, not a terminator, and we
    would have already raised on a previous pass. Here we just match the
    first line whose indentation is <= parent_indent and starts with the
    terminator keyword.
    """
    block_lines: List[str] = []
    i = start
    while i < len(body_lines):
        raw = body_lines[i]
        s = strip_inline_comment(raw)
        if not s:
            block_lines.append(raw)
            i += 1
            continue
        indent = len(raw) - len(raw.lstrip(' '))
        first = s.split()[0] if s.split() else ''
        if indent <= parent_indent and (
            (kind == 'IF' and first == 'ENDIF') or
            (kind == 'FOR' and first == 'NEXT') or
            (kind == 'FUNC' and first == 'ENDFUNC')
        ):
            # Found terminator. ELIF/ELSE handling for IF:
            if kind == 'IF' and first == 'ELSE':
                # We split the IF into multiple statements — but to keep the
                # AST simple, treat ELSE as an unsupported block boundary.
                # User can split it into nested IFs; the compiler still folds
                # constant IFs at resolve time.
                block_lines.append(raw)
                i += 1
                continue
            return parse_body(block_lines), i
        block_lines.append(raw)
        i += 1
    raise ValueError(f'no terminator for {kind} starting at line {start}')


# --- constant folding ---


def _split_lhs_rhs(token: str) -> Tuple[str, str]:
    """For tokens like '进图时间*1000', split to ('进图时间', '*1000')."""
    m = re.match(r'([一-鿿\w]+)(.*)$', token)
    if not m:
        return (token, '')
    return (m.group(1), m.group(2))


def eval_const_expr(text: str, variables: Dict[str, Any], options: Dict[str, Any]) -> Any:
    """Evaluate an expression that uses variables + options + arithmetic.

    Supports integers, + - * / parentheses. Stops at first non-foldable
    element and returns None (caller treats the statement as runtime).
    Bare Chinese identifiers that survive the substitution pass are wrapped
    in quotes so that scripts like `IF 武器发动模式 == 长按连射` fold cleanly
    without forcing the user to write quotes in the source.
    """
    expr = text.strip().replace('，', ',').replace('（', '(').replace('）', ')')
    # Substitute variables first.
    for name, value in list(variables.items()):
        # Numeric vars become integers; option/strings stay quoted.
        if isinstance(value, (int, float)):
            pattern = r'\$' + re.escape(name[1:])  # name starts with $
            expr = re.sub(pattern, str(int(value)), expr)
    for name, value in options.items():
        if isinstance(value, (int, float)):
            expr = re.sub(r'(?<![A-Za-z_一-鿿])' + re.escape(name) +
                          r'(?![A-Za-z_一-鿿])', str(int(value)), expr)
        else:
            expr = re.sub(r'(?<![A-Za-z_一-鿿])' + re.escape(name) +
                          r'(?![A-Za-z_一-鿿])', repr(str(value)), expr)
    # Wrap any remaining bare Chinese identifiers in quotes. The user's
    # scripts use Chinese words like 是/否/长按连射 as literal string
    # values on the right of comparisons.
    expr = re.sub(r'(?<![\'"\w_$])([一-鿿][一-鿿\w]*)(?![\'"\w_])',
                  lambda m: repr(m.group(1)), expr)
    try:
        return eval(expr, {'__builtins__': {}}, {})
    except Exception:
        return None


def fold_statements(stmts: List[Statement], variables: Dict[str, Any],
                    options: Dict[str, Any]) -> List[Statement]:
    """Walk statements, folding $assign with constant RHS, IF with constant
    condition, WAIT with constant expression, etc. Returns the linearised list."""
    out: List[Statement] = []
    for st in stmts:
        if st.kind == 'wait':
            if st.duration_ms is None:
                value = eval_const_expr(st.text, variables, options)
                if value is None:
                    sys.stderr.write(
                        f'WARNING: runtime WAIT expression skipped '
                        f'(text={st.text!r}); not constant-foldable.\n')
                    continue
                st.duration_ms = int(value)
            out.append(st)
            continue
        if st.kind == 'assign':
            value = eval_const_expr(st.rhs_text, variables, options)
            if value is None:
                # Runtime assignment — skip silently. The only side effect is
                # potentially affecting later IF/print, which become runtime.
                continue
            if st.op == '=':
                variables[st.var] = value
            elif st.op == '+=':
                variables[st.var] = variables.get(st.var, 0) + value
            continue
        if st.kind == 'if':
            folded = fold_one_if(st, variables, options)
            out.extend(folded)
            continue
        if st.kind == 'if_chain':
            folded_if = fold_if_chain(st, variables, options)
            out.extend(folded_if)
            continue
        if st.kind == 'for':
            # Compile-time: emit body once. Firmware loops via MacroEngine
            # repeat=true.
            out.extend(fold_statements(st.block, dict(variables), options))
            continue
        if st.kind == 'call':
            # Caller resolved earlier (resolve_calls passes the inlined body).
            out.append(st)
            continue
        if st.kind == 'return':
            continue  # ignored — funcs are inlined
        if st.kind == 'unsupported':
            sys.stderr.write(f'WARNING: {st.text} (line: {st.raw!r})\n')
            continue
        out.append(st)
    return out


def fold_if_chain(stmt: Statement, variables: Dict[str, Any],
                  options: Dict[str, Any]) -> List[Statement]:
    """Pick the matching branch of an IF/ELIF/ELSE chain. branches[0] carries
    the IF's condition; branches[1:] carry ELIF (cond) or ELSE (cond=None)."""
    head = stmt.branches[0]['cond']
    head_lhs = eval_const_expr(head[0], variables, options)
    head_rhs = eval_const_expr(head[2], variables, options)
    head_match = None
    if head_lhs is not None and head_rhs is not None:
        head_match = _compare(head_lhs, head[1], head_rhs)

    if head_match is True:
        return fold_statements(stmt.branches[0]['block'], dict(variables), options)
    if head_match is False:
        # Walk ELIF/ELSE branches.
        for br in stmt.branches[1:]:
            cond = br['cond']
            if cond is None:
                return fold_statements(br['block'], dict(variables), options)
            lhs = eval_const_expr(cond[0], variables, options)
            rhs = eval_const_expr(cond[2], variables, options)
            if lhs is None or rhs is None:
                continue
            if _compare(lhs, cond[1], rhs) is True:
                return fold_statements(br['block'], dict(variables), options)
        return []

    # Head condition didn't fold; conservatively skip the whole IF chain.
    sys.stderr.write(
        f'WARNING: runtime IF skipped at compile (cond={head!r});'
        f' set the option default to the desired branch.\n')
    return []


def _compare(lhs, op, rhs) -> Optional[bool]:
    if op == '==':
        return lhs == rhs
    if op == '!=':
        return lhs != rhs
    if op == '>':
        return lhs > rhs
    if op == '<':
        return lhs < rhs
    if op == '>=':
        return lhs >= rhs
    if op == '<=':
        return lhs <= rhs
    return None


def fold_one_if(stmt: Statement, variables: Dict[str, Any],
                options: Dict[str, Any]) -> List[Statement]:
    """Constant-fold an IF statement.

    The IF condition `lhs op rhs` is folded when both sides reduce to
    constants. Otherwise the IF is passed through unchanged (and the
    underlying statement list will contain a runtime IF, which the
    emitter treats as no-op — see emit_steps).
    """
    lhs = eval_const_expr(stmt.cond_lhs, variables, options)
    rhs = eval_const_expr(stmt.cond_rhs, variables, options)
    if lhs is None or rhs is None:
        # Conservatively, runtime IFs are skipped at compile time. The user's
        # scripts we have so far never use one without folding; if one comes
        # up the warning will surface in the noisy FOR body.
        sys.stderr.write(
            f'WARNING: runtime IF skipped at compile (lhs={stmt.cond_lhs!r}, '
            f'rhs={stmt.cond_rhs!r}); set the option default to the desired '
            f'branch.\n')
        return []
    res = None
    if stmt.cond_op == '==':
        res = lhs == rhs
    elif stmt.cond_op == '!=':
        res = lhs != rhs
    elif stmt.cond_op == '>':
        res = lhs > rhs
    elif stmt.cond_op == '<':
        res = lhs < rhs
    elif stmt.cond_op == '>=':
        res = lhs >= rhs
    elif stmt.cond_op == '<=':
        res = lhs <= rhs
    if res:
        return fold_statements(stmt.block, dict(variables), options)
    return []


def resolve_calls(stmts: List[Statement], functions: Dict[str, List[Statement]]) -> List[Statement]:
    """Replace every CALL with the corresponding FUNC body, recursively."""
    out: List[Statement] = []
    for st in stmts:
        if st.kind == 'call':
            if st.name not in functions:
                raise ValueError(f'CALL to undefined function: {st.name!r}')
            inlined = resolve_calls(functions[st.name], functions)
            out.extend(inlined)
            continue
        if st.kind in ('if', 'for', 'func_def'):
            new_block = resolve_calls(st.block, functions)
            if st.kind == 'func_def':
                continue  # drop FUNC defs — their bodies are inlined at CALL
            new_st = Statement(**{**st.__dict__, 'block': new_block})
            out.append(new_st)
            continue
        out.append(st)
    return out


# --- emission ---


@dataclass
class EmitState:
    """Current controller-report snapshot that the next frame inherits."""
    buttons: int = 0
    dpad: int = DPAD_CENTERED
    lx: int = AXIS_CENTERED
    ly: int = AXIS_CENTERED
    rx: int = AXIS_CENTERED
    ry: int = AXIS_CENTERED


def emit_steps(stmts: List[Statement]) -> List[Step]:
    """Walk folded statements and produce MacroStep entries.

    Emission rules:

    - Each Step is created at the moment we encounter a duration. WAIT,
      button-press with N, and stick-set with N all set the duration for
      the current snapshot. Trigger DOWN/UP / stick-set without N /
      stick-RESET update the snapshot but produce no frame (the next
      duration-bearing instruction does).

    - Button-press with N (e.g. X 50) auto-releases the button after the
      frame, so the next instruction starts from a baseline that does NOT
      still hold X. Triggers (ZL_DOWN / ZR_DOWN) instead persist until a
      matching UP — they keep ZL/ZR pressed across whatever the user does
      next.
    """
    state = EmitState()
    steps: List[Step] = []
    for st in stmts:
        if st.kind == 'wait':
            steps.append(Step(st.duration_ms,
                              state.buttons, state.dpad,
                              state.lx, state.ly, state.rx, state.ry))
        elif st.kind == 'press':
            state.buttons |= BUTTON_BITS[st.button]
            steps.append(Step(st.duration_ms or 0,
                              state.buttons, state.dpad,
                              state.lx, state.ly, state.rx, state.ry))
            # Auto-release after the press unless ZL/ZR (those use DOWN/UP).
            if st.button not in ('ZL', 'ZR'):
                state.buttons &= ~BUTTON_BITS[st.button]
        elif st.kind == 'btn_state':
            bit = BUTTON_BITS[st.button]
            if st.hold:
                state.buttons |= bit
            else:
                state.buttons &= ~bit
        elif st.kind == 'dpad_press':
            # Auto-released directional press for N ms.
            state.dpad = DPAD_DIR_BARE[st.direction]
            steps.append(Step(st.duration_ms or 0,
                              state.buttons, state.dpad,
                              state.lx, state.ly, state.rx, state.ry))
            state.dpad = DPAD_CENTERED
        elif st.kind == 'stick_set':
            lx, ly = STICK_DIRS[st.direction]
            if st.stick == 'L':
                state.lx, state.ly = lx, ly
            else:
                state.rx, state.ry = lx, ly
            if st.duration_ms:
                steps.append(Step(st.duration_ms,
                                  state.buttons, state.dpad,
                                  state.lx, state.ly, state.rx, state.ry))
                # Reset to center at the end of an explicit-duration stick
                # command — matches the only-stick-active convention in the
                # 仅爬塔 等脚本 (set direction, hold for N ms, then return
                # to centered for the next instruction).
                if st.stick == 'L':
                    state.lx, state.ly = AXIS_CENTERED, AXIS_CENTERED
                else:
                    state.rx, state.ry = AXIS_CENTERED, AXIS_CENTERED
        elif st.kind == 'stick_set_state':
            lx, ly = STICK_DIRS[st.direction]
            if st.stick == 'L':
                state.lx, state.ly = lx, ly
            else:
                state.rx, state.ry = lx, ly
            if getattr(st, 'release', False):
                if st.stick == 'L':
                    state.lx, state.ly = AXIS_CENTERED, AXIS_CENTERED
                else:
                    state.rx, state.ry = AXIS_CENTERED, AXIS_CENTERED
        elif st.kind == 'stick_reset':
            if st.stick == 'L':
                state.lx, state.ly = AXIS_CENTERED, AXIS_CENTERED
            else:
                state.rx, state.ry = AXIS_CENTERED, AXIS_CENTERED
        # 'print' / 'assign' / 'if' (folded out) / 'call' (inlined earlier)
        # contribute nothing here.
    # Drop any zero-duration frames that may have sneaked in (press without
    # an explicit ms would emit one — but we already require ms above).
    return [s for s in steps if s.duration_ms > 0]


# --- header / index writer ---


HEADER_TEMPLATE = '''\
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ControllerReport.h"
#include "MacroEngine.h"

namespace farmers {{

// {description} (compiled from {source_basename})
//
// Build: {step_count} steps, total {total_ms} ms (one cycle; firmware loops).

inline constexpr farmers::MacroStep k{Name}Macro[] = {{{steps}}};

inline constexpr size_t k{Name}StepCount =
    sizeof(k{Name}Macro) / sizeof(k{Name}Macro[0]);
inline constexpr uint32_t k{Name}DurationMs = {total_ms};
inline constexpr uint32_t k{Name}LoopGapMs = 0;
inline constexpr uint32_t k{Name}CycleMs = k{Name}DurationMs + k{Name}LoopGapMs;

}}  // namespace farmers
'''


def render_step_lines(steps: List[Step]) -> str:
    parts = []
    for i, s in enumerate(steps):
        terminator = ',' if i < len(steps) - 1 else ''
        parts.append(
            f'    {{{s.duration_ms}, {{{s.buttons:#06x}, '
            f'{s.dpad}, {s.lx}, {s.ly}, {s.rx}, {s.ry}}}}}{terminator}'
        )
    return '\n'.join(parts)


def write_header(name: str, source_basename: str, description: str,
                 steps: List[Step], dest_dir: str) -> str:
    """Render the .h file, write it, return the path."""
    total_ms = sum(s.duration_ms for s in steps)
    body = HEADER_TEMPLATE.format(
        Name=name,
        description=description or '(no description provided)',
        source_basename=source_basename,
        step_count=len(steps),
        total_ms=total_ms,
        steps=render_step_lines(steps),
    )
    out_path = os.path.join(dest_dir, f'Script_{name}.h')
    with open(out_path, 'w', encoding='utf-8') as fh:
        fh.write(body)
    return out_path


INDEX_TEMPLATE = '''\
// AUTO-GENERATED by scripts/compile_macro.py — do not edit.
// Lists every script compiled under firmware/include/Script_*.h.
// Adding a new script: drop scripts/macros/<key>.txt, run the compiler,
// the line below expands automatically.

{farms_per_script}

namespace farmers {{
struct CompiledScript {{
  const char* key;
  const char* label;
  const char* description;
  const farmers::MacroStep* steps;
  size_t step_count;
  uint32_t cycle_ms;
}};

inline constexpr CompiledScript kCompiledScripts[] = {{{entries}}};

inline constexpr size_t kCompiledScriptCount =
    sizeof(kCompiledScripts) / sizeof(kCompiledScripts[0]);

}}  // namespace farmers
'''


def write_index(scripts: List[Dict[str, Any]], dest_path: str) -> str:
    farms = []
    entries = []
    for s in scripts:
        farms.append(
            f'#include "{s["header_basename"]}"'
        )
        entry_steps = s['name']  # MacroStep array identifier
        # Use bare array name (matches kMaterialFarmMacro convention).
        entries.append(
            f'    {{"{s["key"]}", "{s["label"]}", "{s["description_escaped"]}", '
            f'k{s["name"]}Macro, k{s["name"]}StepCount, k{s["name"]}CycleMs}}'
        )
    body = INDEX_TEMPLATE.format(
        farms_per_script='\n'.join(farms),
        entries=',\n'.join(entries),
    )
    with open(dest_path, 'w', encoding='utf-8') as fh:
        fh.write(body)
    return dest_path


# --- top-level driver ---


def sanitize_label_for_filename(label: str) -> str:
    """Convert a script title to a C++ identifier-safe name.

    Falls back to a hash-based slug for non-ASCII titles because C++
    identifiers are restricted to ASCII in our PlatformIO toolchain.
    Callers that want a display label should use the original `title`
    instead — this is only the embedded array name.
    """
    s = re.sub(r'[^A-Za-z0-9]+', '_', label).strip('_')
    if not s:
        s = 'Script'
    return s[0].upper() + s[1:]


def compile_text(text: str, key: str, source_basename: str) -> Dict[str, Any]:
    meta, body = parse_top_lines(text)
    title = meta['title']
    description = meta['description'] or title or key
    label = title or key
    # The C++ name needs to be (a) ASCII-only because our PlatformIO
    # toolchain rejects Unicode identifiers and (b) globally unique even
    # when several scripts share a Chinese title (e.g. four scripts
    # whose title is the boilerplate 涂击队62级跑图). Source_basename
    # has Unicode too, so we hash it. The picker UI uses `label` (the
    # human-readable Chinese title) for display; this `name` is the
    # embedded array identifier only.
    import hashlib
    digest = hashlib.md5(source_basename.encode('utf-8')).hexdigest()[:6]
    name = f'Script_{digest}'
    # Numeric options default fold:
    options = {}
    options.update({k: v[0] for k, v in meta['numeric_options'].items()})
    options.update({k: v[0] if v else '' for k, v in meta['list_options'].items()})
    variables: Dict[str, Any] = {}
    raw_stmts = parse_body(body, source_name=source_basename)
    # Hoist FUNC defs out, replace CALL with inline.
    functions: Dict[str, List[Statement]] = {}
    top_stmts: List[Statement] = []
    for st in raw_stmts:
        if st.kind == 'func_def':
            functions[st.name] = st.block
            continue
        top_stmts.append(st)
    inlined = resolve_calls(top_stmts, functions)
    folded = fold_statements(inlined, dict(variables), options)
    steps = emit_steps(folded)
    return {
        'key': key,
        'name': name,
        'label': label,
        'title': title,
        'description': description,
        'description_escaped': description.replace('"', '\\"'),
        'header_basename': f'Script_{name}.h',
        'step_count': len(steps),
        'total_ms': sum(s.duration_ms for s in steps),
        'steps': steps,
    }


def compile_file(path: str, include_dir: str) -> Dict[str, Any]:
    with open(path, 'r', encoding='utf-8') as fh:
        text = fh.read()
    key = os.path.splitext(os.path.basename(path))[0]
    info = compile_text(text, key, source_basename=os.path.basename(path))
    write_header(info['name'], os.path.basename(path), info['description'],
                 info['steps'], include_dir)
    return info


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('paths', nargs='+', help='files or directories to compile')
    ap.add_argument('--include', default='firmware/include',
                    help='output directory for .h files (default firmware/include)')
    ap.add_argument('--index', default='firmware/include/scripts_index.inc',
                    help='output path for scripts_index.inc')
    args = ap.parse_args()

    scripts: List[Dict[str, Any]] = []
    targets: List[str] = []
    for p in args.paths:
        if os.path.isdir(p):
            for name in sorted(os.listdir(p)):
                if name.endswith('.txt'):
                    targets.append(os.path.join(p, name))
        elif os.path.isfile(p):
            targets.append(p)
        else:
            sys.stderr.write(f'skip: not found {p}\n')

    for path in targets:
        try:
            info = compile_file(path, args.include)
        except Exception as e:
            sys.stderr.write(f'FAILED {path}: {e}\n')
            return 1
        sys.stderr.write(
            f'compiled {path} -> {info["header_basename"]} '
            f'({info["step_count"]} steps, '
            f'{info["total_ms"] / 1000:.1f}s)\n'
        )
        scripts.append(info)

    if not scripts:
        sys.stderr.write('no scripts to compile\n')
        return 1

    write_index(scripts, args.index)
    sys.stderr.write(f'wrote {args.index}\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
