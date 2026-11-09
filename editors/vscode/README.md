# openc166 debugger for VS Code

Runs C166/C167 firmware in the openc166 simulator and debugs it through the
Debug Adapter Protocol (`python -m openc166.dap`).

Install: `pip install openc166`, then copy or symlink this folder to
`~/.vscode/extensions/openc166-debug` and reload VS Code. Set
`openc166.python` if the package lives in another interpreter.

Example `.vscode/launch.json`:

```json
{
  "configurations": [
    {
      "type": "openc166", "request": "launch", "name": "Stock 5WY19",
      "image": "${workspaceFolder}/ca663057.bin", "board": "5wy19",
      "ghidraSymbols": "${workspaceFolder}/ca663057.symbols.json",
      "watchdogFrozen": true
    },
    {
      "type": "openc166", "request": "launch", "name": "My payload",
      "program": "${workspaceFolder}/payload.a66", "board": "c167"
    }
  ]
}
```

Step back and reverse continue work from the debug toolbar. Data
breakpoints: add an expression such as `0xFC40`, `S0TBUF` or `PSW.Z` to the
Watch view and choose "Break on Value Change". The Debug Console takes
register and SFR names, symbols, `[address]`, or Python with `s` bound to
the simulator.

Neovim (nvim-dap):

```lua
require("dap").adapters.openc166 = { type = "executable", command = "python3", args = { "-m", "openc166.dap" } }
```
