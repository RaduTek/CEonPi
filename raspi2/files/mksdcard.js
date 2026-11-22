// mksdcard.js - builds the SD card folder in the flat release directory.
//
// Called by PostMakeimg.bat (cscript //nologo mksdcard.js) after romimage.
// Creates %_FLATRELEASEDIR%\sdcard from scratch, a complete set of files for a
// blank FAT32 SD card:
//   - every file in sdboot (raspi2\files\sdboot, copied to the release dir by
//     Copy Files to Release Directory): the Raspberry Pi firmware
//     (bootcode.bin, start.elf, fixup.dat, the cut-down start_cd.elf /
//     fixup_cd.dat), LICENCE.broadcom and our config.txt.
//   - sdcard\kernel.img : a copy of nk.nb0 (nk.nb0 itself stays in place).
//   - with BSP_SDROOT=1 : the folders and files that initobj.dat lists for
//     the root file system (desktop shortcuts, \Windows\Desktop,
//     \Windows\Programs, \Program Files, \My Documents, \Windows\Keys, ...).
//     With the SD card as root, filesys applies initobj.dat to the RAM object
//     store before the card is mounted, so these never reach the card by
//     themselves. Files named in initobj.dat (File("name","\Windows\src")) are
//     copied from the release dir by their source file name.
//   - with BSP_SDOFFLOAD=1 : sdcard\Windows gets the modules that sdoffload.js
//     removed from the image (listed in sdoffload.txt).
// Copy the CONTENTS of sdcard to the root of the SD card. See docs\sdroot.md.
//
// Exit code 0 = ok, 1 = error (makeimg then reports a failure).

var fso = new ActiveXObject("Scripting.FileSystemObject");
var env = new ActiveXObject("WScript.Shell").Environment("Process");

function log(s) { WScript.Echo("mksdcard: " + s); }
function fail(s) { WScript.Echo("mksdcard: ERROR: " + s); WScript.Quit(1); }

// Creates a folder and any missing parents.
function ensureFolder(path) {
    if (fso.FolderExists(path)) return;
    var parent = fso.GetParentFolderName(path);
    if (parent && !fso.FolderExists(parent)) ensureFolder(parent);
    fso.CreateFolder(path);
}

// Maps a CE path ("\Windows\Desktop", "Windows", "") to a folder under sdcard.
function sdPath(root, cePath) {
    var p = cePath.replace(/^\\+/, "").replace(/\\+$/, "");
    return p ? fso.BuildPath(root, p) : root;
}

var frd = env("_FLATRELEASEDIR");
if (!frd || !fso.FolderExists(frd)) fail("_FLATRELEASEDIR is not set or does not exist");

// Modules moved out of the image (BSP_SDOFFLOAD). sdoffload.js wrote
// sdoffload.txt when ce.bib was last rebuilt: ON/OFF, then the removed
// modules. makeimg only rebuilds ce.bib when a .bib file changes, not when a
// catalog item (environment variable) changes, so a mismatch means the image
// was built with the old setting.
var offloadOn = (env("BSP_SDOFFLOAD") == "1");
var offloadList = [];
var offloadState = "OFF";
var listFile = fso.BuildPath(frd, "sdoffload.txt");
if (fso.FileExists(listFile)) {
    var lts = fso.OpenTextFile(listFile, 1, false, 0);
    if (!lts.AtEndOfStream) offloadState = lts.ReadLine();
    while (!lts.AtEndOfStream) {
        var n = lts.ReadLine();
        if (n) offloadList.push(n);
    }
    lts.Close();
}
if (offloadState != (offloadOn ? "ON" : "OFF")) {
    fail("BSP_SDOFFLOAD (\"Load optional modules from the SD card\") changed since ce.bib was " +
         "built. Delete ce.bib in " + frd + " and run Make Run-Time Image again.");
}

var sd = fso.BuildPath(frd, "sdcard");
if (fso.FolderExists(sd)) {
    try { fso.DeleteFolder(sd, true); }
    catch (e) { fail("cannot delete " + sd + " (is a file in it open?)"); }
}
ensureFolder(sd);

// Firmware, licence and config.txt from sdboot. The first four are needed to
// boot at all; the rest of the folder is copied as it is.
var boot = fso.BuildPath(frd, "sdboot");
var required = ["bootcode.bin", "start.elf", "fixup.dat", "config.txt"];
if (!fso.FolderExists(boot)) fail(boot + " not found (run Copy Files to Release Directory)");
for (var i = 0; i < required.length; i++) {
    if (!fso.FileExists(fso.BuildPath(boot, required[i]))) fail(required[i] + " missing from " + boot);
}
var nBoot = 0;
for (var e = new Enumerator(fso.GetFolder(boot).Files); !e.atEnd(); e.moveNext()) {
    fso.CopyFile(e.item().Path, fso.BuildPath(sd, e.item().Name), true);
    nBoot++;
}
log(nBoot + " boot files copied from sdboot");

// kernel.img
var nb0 = fso.BuildPath(frd, "nk.nb0");
if (!fso.FileExists(nb0)) fail("nk.nb0 not found in " + frd);
fso.CopyFile(nb0, fso.BuildPath(sd, "kernel.img"), true);
log("kernel.img copied from nk.nb0");

// initobj.dat items, only when the SD card is the root file system.
if (env("BSP_SDROOT") == "1") {
    var initobj = fso.BuildPath(frd, "initobj.dat");
    if (!fso.FileExists(initobj)) fail("initobj.dat not found in " + frd);

    // initobj.dat is UTF-16LE (no BOM); -1 = open as Unicode.
    var ts = fso.OpenTextFile(initobj, 1, false, -1);
    var re = /^\s*(root|Directory\("([^"]*)"\))\s*:-\s*(Directory|File)\("([^"]*)"(\s*,\s*"([^"]*)")?\)/i;
    var nDirs = 0, nFiles = 0, nSkipped = 0;

    while (!ts.AtEndOfStream) {
        var line = ts.ReadLine();
        if (/^\s*(;|$)/.test(line)) continue;

        var m = re.exec(line);
        if (!m) { log("skipped unrecognised line: " + line); nSkipped++; continue; }

        var parent = (m[1].toLowerCase() == "root") ? "" : m[2];
        var parentDir = sdPath(sd, parent);
        ensureFolder(parentDir);

        if (m[3].toLowerCase() == "directory") {
            ensureFolder(fso.BuildPath(parentDir, m[4]));
            nDirs++;
        } else {
            var srcName = fso.GetFileName(m[6] ? m[6] : m[4]);
            var src = fso.BuildPath(frd, srcName);
            if (!fso.FileExists(src)) { log("WARNING: " + srcName + " not in release dir, skipped"); nSkipped++; continue; }
            fso.CopyFile(src, fso.BuildPath(parentDir, m[4]), true);
            nFiles++;
        }
    }
    ts.Close();
    log("initobj.dat: " + nDirs + " folders, " + nFiles + " files, " + nSkipped + " skipped");
}

// Modules removed from the image by sdoffload.js go to the card's Windows
// folder; with the SD card as root they appear in \Windows.
if (offloadOn) {
    var winDir = fso.BuildPath(sd, "Windows");
    ensureFolder(winDir);
    for (var i = 0; i < offloadList.length; i++) {
        var msrc = fso.BuildPath(frd, offloadList[i]);
        if (!fso.FileExists(msrc)) fail(offloadList[i] + " (moved out of the image) not found in " + frd);
        fso.CopyFile(msrc, fso.BuildPath(winDir, offloadList[i]), true);
    }
    log(offloadList.length + " modules moved out of the image copied to Windows");
}

log("done: copy the contents of " + sd + " to the root of the SD card");
WScript.Quit(0);
