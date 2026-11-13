"""Local co-op (runtime/hle/coop.c): every read of "Link" in the game's main program, for tools/patches.py.

"Link" to the game is its game manager's player slot (GAME_MGR -> +0x4b8). With co-op the enemies' reads of it have to
give the player each enemy goes for; tools/patches.py replaces each read listed here with rt_coop_player(), which with
co-op off - or for any other read - gives back what the slot holds. Listed: every `ldr xT, [xN, #0x4b8]` (the 64-bit
load at that offset; xN not sp, xT not xzr). Most have the game manager in xN, as a pass through each function sees it
(an adrp of GAME_MGR's page and a load of it, then moves; "gm" in the list); for the others ("?") xN may be another
object with something else at +0x4b8, so the runtime checks the base itself at every read. Pairs (ldp) reaching +0x4b8
and loads with a register offset aren't listed: this prints how many ldp there are, to look at by hand.

Usage: python tools/coop_player_reads.py <extracted>/exefs/main [out]
       (out: runtime/game_addresses/coop_player_reads_<version>.txt, the version from the program's build ID)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import analyze  # noqa: E402
import game_addresses  # noqa: E402
from nso import Nso  # noqa: E402

MAIN_BASE = 0x7000000000
SLOT = 0x4b8


def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def scan(n, gm):
    """[(offset, word, tracked)] for every ldr xT, [xN, #0x4b8]; and the ldp reaching +0x4b8."""
    funcs, words = analyze.discover(n)
    starts = sorted(funcs)
    lo, hi = n.text[0], n.text[0] + n.text[1]
    out, pairs = [], []
    for fi, f in enumerate(starts):
        end = starts[fi + 1] if fi + 1 < len(starts) else hi
        page, addr_of, mgr = {}, set(), set()

        def forget(r):
            page.pop(r, None)
            addr_of.discard(r)
            mgr.discard(r)

        for a in range(f, end, 4):
            w = words[(a - lo) // 4]
            rd, rn = w & 31, (w >> 5) & 31
            if (w & 0x9F000000) == 0x90000000:  # adrp
                imm = ((w >> 29) & 3) | (((w >> 5) & 0x7FFFF) << 2)
                forget(rd)
                page[rd] = (a & ~0xFFF) + (sx(imm, 21) << 12)
                continue
            if (w & 0xFFC00000) == 0x91000000 and rn in page:  # add xd, xn, #imm (an adrp's page)
                v = page[rn] + ((w >> 10) & 0xFFF)
                forget(rd)
                if v == gm:
                    addr_of.add(rd)
                continue
            if (w & 0xFFC00000) == 0xF9400000:  # ldr xT, [xN, #imm]
                off = ((w >> 10) & 0xFFF) * 8
                if off == SLOT and rn != 31 and rd != 31:
                    out.append((a, w, rn in mgr))
                kind = None
                if rn in page:
                    v = page[rn] + off
                    if v == gm:
                        kind = 'mgr'
                    elif 0 <= v <= len(n.image) - 8 and n.u64(v) == gm:  # (a GOT entry: GAME_MGR's address)
                        kind = 'addr'
                elif rn in addr_of and off == 0:
                    kind = 'mgr'
                forget(rd)
                if kind == 'mgr':
                    mgr.add(rd)
                elif kind == 'addr':
                    addr_of.add(rd)
                continue
            if (w & 0x7FC00000) == 0x29400000 or (w & 0x7FC00000) == 0x29C00000 or (w & 0x7FC00000) == 0x28C00000:
                # ldp (signed offset / pre / post index): x registers, its two loads at imm7 * 8 and + 8
                if w >> 31:
                    o = sx((w >> 15) & 0x7F, 7) * 8
                    if (o == SLOT or o + 8 == SLOT) and rn != 31:
                        pairs.append((a, w, rn in mgr))
            if (w & 0xFFE0FFE0) == 0xAA0003E0:  # mov xd, xm
                rm = (w >> 16) & 31
                m, ad = rm in mgr, rm in addr_of
                forget(rd)
                if m:
                    mgr.add(rd)
                if ad:
                    addr_of.add(rd)
                continue
            if (w & 0xFC000000) == 0x94000000 or (w & 0xFFFFFC1F) == 0xD63F0000:  # bl / blr: caller-saved gone
                for r in range(19):
                    forget(r)
                continue
            op = (w >> 25) & 0xF
            if (op & 0b1110) == 0b1000 or (op & 0b0111) == 0b0101:  # data processing: writes xd
                forget(rd)
            elif (op & 0b0101) == 0b0100 and (w >> 22) & 1:  # a load: writes xT (and xT2 for a pair)
                forget(rd)
                if (w >> 27) & 7 == 0b101:
                    forget((w >> 10) & 31)
    return out, pairs


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    n = Nso(sys.argv[1])
    key = game_addresses.select_build_id(n.build_id)
    gm = game_addresses.ADDRESS.GAME_MGR - MAIN_BASE if hasattr(game_addresses.ADDRESS, 'GAME_MGR') else 0
    if not gm:  # (GAME_MGR is a runtime constant: runtime_<key>.h)
        t = game_addresses.load_table(game_addresses.table_path(key, 'runtime'))
        gm = t['GAME_MGR'] - MAIN_BASE
    reads, pairs = scan(n, gm)
    path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), '..', 'runtime', 'game_addresses', f'coop_player_reads_{key}.txt')
    with open(path, 'w', newline='\n') as f:
        f.write(f'# Every ldr xT, [xN, #0x4b8] in main ({key}): tools/coop_player_reads.py made this list; tools/patches.py\n'
                f'# replaces each with rt_coop_player() (runtime/hle/coop.c). <address> <instruction word> <gm: xN holds\n'
                f'# the game manager as a pass through the function sees it; ?: not known>\n')
        for a, w, t in reads:
            f.write(f'{MAIN_BASE + a:#x} {w:#010x} {"gm" if t else "?"}\n')
    print(f'{key}: {len(reads)} reads of +0x4b8 ({sum(t for _, _, t in reads)} from the game manager), '
          f'{len(pairs)} ldp reaching +0x4b8 ({sum(t for _, _, t in pairs)} from the game manager) -> {path}')
    for a, w, t in pairs:
        print(f'  ldp at {MAIN_BASE + a:#x} {w:#010x} {"gm" if t else "?"}')


if __name__ == '__main__':
    main()
