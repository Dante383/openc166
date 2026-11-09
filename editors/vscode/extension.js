// SPDX-License-Identifier: MIT
// Starts the openc166 debug adapter (python -m openc166.dap) for "openc166"
// launch configurations.
const vscode = require("vscode");

function activate(context) {
  context.subscriptions.push(
    vscode.debug.registerDebugAdapterDescriptorFactory("openc166", {
      createDebugAdapterDescriptor() {
        const python = vscode.workspace.getConfiguration("openc166").get("python", "python3");
        return new vscode.DebugAdapterExecutable(python, ["-m", "openc166.dap"]);
      },
    })
  );
}

function deactivate() {}

module.exports = { activate, deactivate };
