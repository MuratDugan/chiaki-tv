"""ARM NaCl SFI kurallarinin kaba denetimi (resmi ncval yerine gecmez): maskelenmemis
bellek erisimi ve dolayli dallanma arar, bulunduklari islevlere gore sayar."""
import re, subprocess, sys, collections
objdump, nexe = sys.argv[1], sys.argv[2]
p = subprocess.Popen([objdump, "-d", "--no-show-raw-insn", nexe], stdout=subprocess.PIPE, text=True, errors="replace")
sym = "?"; prev = (None, "", ""); bad = collections.Counter(); samples = {}; total = 0; mem = 0; ind = 0
insn_re = re.compile(r"^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$")
for line in p.stdout:
    m = re.match(r"^[0-9a-f]+ <(.+)>:$", line)
    if m: sym = m.group(1); prev = (None, "", ""); continue
    m = insn_re.match(line)
    if not m: continue
    addr = int(m.group(1), 16); mn = m.group(2); ops = m.group(3).split(";")[0].strip()
    total += 1
    if mn.startswith(".") or mn in ("bkpt", "nop"):
        prev = (addr, mn, ops); continue
    why = None
    b = re.search(r"\[(r\d+|sl|fp|ip|lr|sp|pc)\b", ops)
    is_mem = b and re.match(r"(ldr|str|ldm|stm|vldr|vstr|vld|vst|ldrex|strex|pld|push|pop)", mn)
    if is_mem:
        base = b.group(1); mem += 1
        if base not in ("sp", "pc", "r9"):
            pa, pm, po = prev
            same = pa is not None and pa == addr - 4 and (pa & ~0xF) == (addr & ~0xF)
            masked = same and pm.startswith("bic") and re.match(r"%s, %s, #-1073741824" % (base, base), po)
            tested = same and pm.startswith("tst") and re.match(r"%s, #-1073741824" % base, po) and mn.endswith("eq")
            if not (masked or tested) and not mn.startswith("pld"):
                why = "maskesiz bellek erisimi"
    elif re.match(r"(bx|blx)", mn) and re.match(r"^(r\d+|sl|fp|ip|lr)$", ops):
        ind += 1
        pa, pm, po = prev
        same = pa is not None and pa == addr - 4 and (pa & ~0xF) == (addr & ~0xF)
        if not (same and pm.startswith("bic") and re.match(r"%s, %s, #-1073741809" % (ops, ops), po)):
            why = "maskesiz dolayli dallanma"
    elif mn.startswith(("svc", "swi")):
        why = "sistem cagrisi"
    if why:
        bad[(sym, why)] += 1
        samples.setdefault((sym, why), "%x: %s %s" % (addr, mn, ops))
    prev = (addr, mn, ops)
print("komut: %d, bellek erisimi: %d, dolayli dallanma: %d, supheli: %d (%d islevde)" % (total, mem, ind, sum(bad.values()), len(bad)))
for (s, w), n in bad.most_common(25):
    print("  %5d  %-40s %s   ornek: %s" % (n, s[:40], w, samples[(s, w)]))
