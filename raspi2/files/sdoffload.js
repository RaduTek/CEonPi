// sdoffload.js - removes the modules listed for the SD card from ce.bib.
//
// Called by PostFmergeBib.bat, which makeimg runs right after fmerge writes
// ce.bib and before romimage (public\common\oak\misc\makeimg.proj,
// CreateConfigBibFile). That target only runs when a .bib input changed or
// ce.bib is missing.
//
// With BSP_SDOFFLOAD=1 (catalog item "Load optional modules from the SD
// card"; needs BSP_SDROOT=1) every MODULES entry named on a
// "; SDOFFLOAD <name>" comment line (platform.bib) is dropped from ce.bib, so
// romimage leaves it out of the image. mksdcard.js later copies those modules
// into sdcard\Windows; with the SD card as root they appear in \Windows.
//
// Always writes %_FLATRELEASEDIR%\sdoffload.txt: first line ON or OFF (the
// BSP_SDOFFLOAD value this ce.bib was built with), then one removed module
// per line. mksdcard.js uses it to copy the modules and to detect a toggled
// catalog item whose ce.bib was not rebuilt.
//
// Exit code 0 = ok, 1 = error (makeimg then stops).

var fso = new ActiveXObject("Scripting.FileSystemObject");
var env = new ActiveXObject("WScript.Shell").Environment("Process");

function log(s) { WScript.Echo("sdoffload: " + s); }
function fail(s) { WScript.Echo("sdoffload: ERROR: " + s); WScript.Quit(1); }

function writeLines(path, lines) {
    var ts = fso.CreateTextFile(path, true, false);   // overwrite, ASCII
    for (var i = 0; i < lines.length; i++) ts.WriteLine(lines[i]);   // CRLF
    ts.Close();
}

var frd = env("_FLATRELEASEDIR");
if (!frd || !fso.FolderExists(frd)) fail("_FLATRELEASEDIR is not set or does not exist");
var bib = fso.BuildPath(frd, "ce.bib");
var listFile = fso.BuildPath(frd, "sdoffload.txt");

if (env("BSP_SDOFFLOAD") != "1") {
    writeLines(listFile, ["OFF"]);
    log("BSP_SDOFFLOAD not set, ce.bib unchanged");
    WScript.Quit(0);
}
if (env("BSP_SDROOT") != "1") {
    fail("BSP_SDOFFLOAD=1 needs BSP_SDROOT=1 (\"SD card as root file system\"): " +
         "without it nothing loads the modules from the card");
}
if (!fso.FileExists(bib)) fail("ce.bib not found in " + frd);

// Read ce.bib and collect the list.
var lines = [];
var ts = fso.OpenTextFile(bib, 1, false, 0);
while (!ts.AtEndOfStream) lines.push(ts.ReadLine());
ts.Close();

var wanted = {};       // lower-case name -> name as listed
var order = [];
for (var i = 0; i < lines.length; i++) {
    var m = /^\s*;\s*SDOFFLOAD\s+(\S+)/i.exec(lines[i]);
    if (m && !wanted[m[1].toLowerCase()]) {
        wanted[m[1].toLowerCase()] = m[1];
        order.push(m[1].toLowerCase());
    }
}

// Drop the matching entries, only inside the MODULES section.
var out = [];
var removed = {};
var section = "";
for (var i = 0; i < lines.length; i++) {
    var line = lines[i];
    var sec = /^\s*(MEMORY|CONFIG|MODULES|FILES)\s*$/i.exec(line);
    if (sec) section = sec[1].toUpperCase();

    if (section == "MODULES" && !/^\s*(;|$)/.test(line)) {
        var name = line.replace(/^\s+/, "").split(/\s+/)[0].toLowerCase();
        if (wanted[name]) {
            if (removed[name]) fail(name + " is listed twice in the MODULES section of ce.bib");
            removed[name] = true;
            continue;
        }
    }
    out.push(line);
}

// Modules removed by an earlier run on this same ce.bib (if fmerge left the
// filtered file in place) are no longer in it; keep them on the list.
var previous = {};
if (fso.FileExists(listFile)) {
    var pts = fso.OpenTextFile(listFile, 1, false, 0);
    if (!pts.AtEndOfStream && pts.ReadLine() == "ON") {
        while (!pts.AtEndOfStream) {
            var pn = pts.ReadLine();
            if (pn) previous[pn.toLowerCase()] = true;
        }
    }
    pts.Close();
}

var result = ["ON"];
var nRemoved = 0, nAlready = 0;
for (var i = 0; i < order.length; i++) {
    if (removed[order[i]]) {
        result.push(wanted[order[i]]);
        nRemoved++;
    } else if (previous[order[i]]) {
        result.push(wanted[order[i]]);
        nAlready++;
    } else {
        log("WARNING: " + wanted[order[i]] + " is not a module in this image, skipped");
    }
}

// Replace ce.bib through a temporary file.
var tmp = bib + ".sdoffload.tmp";
writeLines(tmp, out);
fso.DeleteFile(bib, true);
fso.MoveFile(tmp, bib);
writeLines(listFile, result);

log("removed " + nRemoved + " of " + order.length + " listed modules from ce.bib" +
    (nAlready ? " (" + nAlready + " more were already removed)" : ""));
WScript.Quit(0);
