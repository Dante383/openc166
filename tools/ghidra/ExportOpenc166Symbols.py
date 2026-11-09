# Exports functions, labels and comments of the current program as JSON for
# the openc166 simulator and debugger (openc166.sim.Symbols.load_ghidra).
#
# Run it from Ghidra's Script Manager (Jython). It asks where to save.
#
# @category openc166
# @runtime Jython

import json

from ghidra.program.model.listing import CodeUnit
from ghidra.program.model.symbol import SourceType, SymbolType


def addr(a):
    return a.getOffset()


program = currentProgram
out = {"program": program.getName(), "functions": [], "labels": [], "comments": []}

for f in program.getFunctionManager().getFunctions(True):
    out["functions"].append({"address": addr(f.getEntryPoint()), "name": f.getName()})

for s in program.getSymbolTable().getAllSymbols(True):
    if s.getSymbolType() != SymbolType.LABEL or s.getSource() == SourceType.DEFAULT:
        continue
    out["labels"].append({"address": addr(s.getAddress()), "name": s.getName()})

listing = program.getListing()
for kind, name in ((CodeUnit.EOL_COMMENT, "eol"), (CodeUnit.PRE_COMMENT, "pre"), (CodeUnit.PLATE_COMMENT, "plate")):
    it = listing.getCommentAddressIterator(kind, program.getMemory(), True)
    while it.hasNext():
        a = it.next()
        text = listing.getComment(kind, a)
        if text:
            out["comments"].append({"address": addr(a), "kind": name, "text": text})

path = askFile("Save openc166 symbols", "Save").getAbsolutePath()
with open(path, "w") as f:
    json.dump(out, f, indent=1)
print("wrote %d functions, %d labels, %d comments to %s"
      % (len(out["functions"]), len(out["labels"]), len(out["comments"]), path))
