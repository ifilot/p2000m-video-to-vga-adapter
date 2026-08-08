// SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
// SPDX-License-Identifier: GPL-3.0-or-later

function Component() {
    // Default component constructor.
}

Component.prototype.createOperations = function() {
    component.createOperations();
    component.addOperation(
        "CreateShortcut",
        "@TargetDir@/p2000m-vid2vga-viewer.exe",
        "@StartMenuDir@/P2000M VID2VGA Viewer.lnk",
        "workingDirectory=@TargetDir@",
        "iconPath=@TargetDir@/p2000m-vid2vga-viewer.exe"
    );
    component.addOperation(
        "CreateShortcut",
        "@TargetDir@/p2000m-vid2vga-viewer.exe",
        "@DesktopDir@/P2000M VID2VGA Viewer.lnk",
        "workingDirectory=@TargetDir@",
        "iconPath=@TargetDir@/p2000m-vid2vga-viewer.exe"
    );
}
