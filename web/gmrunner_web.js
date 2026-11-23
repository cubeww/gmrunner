(function () {
    "use strict";

    // Track all AudioContexts constructed by miniaudio/SDL so the first user
    // gesture reliably unlocks WebAudio across Chrome, Firefox, and Safari.
    const trackedAudioContexts = [];
    const NativeAudioContext = window.AudioContext || window.webkitAudioContext;
    if (NativeAudioContext) {
        const WrappedAudioContext = function (...args) {
            const ctx = new NativeAudioContext(...args);
            trackedAudioContexts.push(ctx);
            return ctx;
        };
        WrappedAudioContext.prototype = NativeAudioContext.prototype;
        window.AudioContext = WrappedAudioContext;
        if (window.webkitAudioContext) {
            window.webkitAudioContext = WrappedAudioContext;
        }
    }

    function unlockTrackedAudio() {
        for (const ctx of trackedAudioContexts) {
            if (ctx && ctx.state === "suspended") {
                ctx.resume().catch(() => {});
            }
        }
    }

    const IGNORABLE_EXES = ["unins", "vcredist", "dxsetup.exe", "crashreporter.exe"];
    function isIgnorableExe(lowerName) {
        return IGNORABLE_EXES.some((p) => lowerName === p || lowerName.startsWith(p));
    }

    function normalizeRelPath(path) {
        return String(path || "")
            .replace(/\\/g, "/")
            .replace(/^\/+/, "")
            .replace(/^\.\//, "")
            .replace(/\/+$/, "");
    }

    // -----------------------------------------------------------------------
    // Persistent storage manager (OPFS primary + Emscripten IDBFS fallback)
    // -----------------------------------------------------------------------
    let activeGameKey = "default";
    let persistBackend = "none"; // "OPFS" | "IDBFS" | "none"
    let persistSyncTimer = null;
    let persistSyncInFlight = null;
    let persistSyncPending = false;
    let idbfsMounted = false;

    async function opfsGetGameDir(gameKey, create) {
        const root = await navigator.storage.getDirectory();
        const savesRoot = await root.getDirectoryHandle("gmrunner_saves", { create });
        return await savesRoot.getDirectoryHandle(gameKey, { create });
    }

    function ensureMemfsDir(path) {
        const parts = path.split("/").filter(Boolean);
        let cur = "";
        for (const p of parts) {
            cur += "/" + p;
            try {
                Module.FS.mkdir(cur);
            } catch (_) {}
        }
    }

    async function copyOpfsDirToMemfs(opfsDir, memfsPath) {
        ensureMemfsDir(memfsPath);
        for await (const [name, handle] of opfsDir.entries()) {
            const target = memfsPath + "/" + name;
            if (handle.kind === "directory") {
                await copyOpfsDirToMemfs(handle, target);
            } else if (handle.kind === "file") {
                const file = await handle.getFile();
                const bytes = new Uint8Array(await file.arrayBuffer());
                Module.FS.writeFile(target, bytes);
            }
        }
    }

    async function copyMemfsDirToOpfs(memfsPath, opfsDir) {
        let entries;
        try {
            entries = Module.FS.readdir(memfsPath);
        } catch (_) {
            return;
        }
        const liveNames = new Set();
        for (const name of entries) {
            if (name === "." || name === "..") continue;
            liveNames.add(name);
            const full = memfsPath + "/" + name;
            const st = Module.FS.stat(full);
            if (Module.FS.isDir(st.mode)) {
                const sub = await opfsDir.getDirectoryHandle(name, { create: true });
                await copyMemfsDirToOpfs(full, sub);
            } else {
                const data = Module.FS.readFile(full);
                const fh = await opfsDir.getFileHandle(name, { create: true });
                const writable = await fh.createWritable();
                await writable.write(data);
                await writable.close();
            }
        }
        // Remove OPFS entries deleted from MEMFS.
        for await (const [existing] of opfsDir.entries()) {
            if (!liveNames.has(existing)) {
                try {
                    await opfsDir.removeEntry(existing, { recursive: true });
                } catch (_) {}
            }
        }
    }

    async function mountPersist(gameKey) {
        activeGameKey = gameKey;
        const persistPath = "/persist/" + activeGameKey;
        const badge = document.getElementById("storage-badge");

        // 1. Prefer Origin Private File System (OPFS).
        if (navigator.storage && typeof navigator.storage.getDirectory === "function") {
            try {
                const dir = await opfsGetGameDir(gameKey, true);
                await copyOpfsDirToMemfs(dir, persistPath);
                persistBackend = "OPFS";
                if (badge) badge.textContent = "Storage: OPFS";
                return true;
            } catch (err) {
                console.warn("[gmrunner] OPFS unavailable, falling back to IDBFS:", err);
            }
        }

        // 2. Automatic fallback to Emscripten IDBFS (IndexedDB).
        try {
            ensureMemfsDir("/persist");
            if (!idbfsMounted) {
                Module.FS.mount(Module.IDBFS, {}, "/persist");
                idbfsMounted = true;
            }
            await new Promise((resolve, reject) => {
                Module.FS.syncfs(true, (err) => (err ? reject(err) : resolve()));
            });
            ensureMemfsDir(persistPath);
            persistBackend = "IDBFS";
            if (badge) badge.textContent = "Storage: IDBFS";
            return true;
        } catch (err) {
            console.warn("[gmrunner] IDBFS mount failed, using ephemeral MEMFS:", err);
            ensureMemfsDir(persistPath);
            persistBackend = "none";
            if (badge) badge.textContent = "Storage: Session";
            return true;
        }
    }

    async function syncPersist(gameKey, backend) {
        do {
            persistSyncPending = false;
            try {
                if (backend === "OPFS") {
                    const dir = await opfsGetGameDir(gameKey, true);
                    await copyMemfsDirToOpfs("/persist/" + gameKey, dir);
                } else {
                    await new Promise((resolve, reject) => {
                        Module.FS.syncfs(false, (err) => err ? reject(err) : resolve());
                    });
                }
            } catch (err) {
                console.warn("[gmrunner] Persist sync warning:", err);
            }
        } while (persistSyncPending);
        persistSyncInFlight = null;
    }

    function flushPersistNow() {
        if (persistBackend === "none") return;
        clearTimeout(persistSyncTimer);
        persistSyncTimer = null;
        persistSyncPending = true;
        if (!persistSyncInFlight) {
            persistSyncInFlight = syncPersist(activeGameKey, persistBackend);
        }
        return persistSyncInFlight;
    }

    function schedulePersistSync() {
        if (persistBackend === "none") return;
        if (persistSyncInFlight) {
            persistSyncPending = true;
            return;
        }
        if (persistSyncTimer !== null) clearTimeout(persistSyncTimer);
        persistSyncTimer = setTimeout(() => {
            persistSyncTimer = null;
            flushPersistNow();
        }, 400);
    }

    // -----------------------------------------------------------------------
    // Minimal uncompressed ZIP builder for "Export Saves" download
    // -----------------------------------------------------------------------
    function buildStoreZip(fileEntries) {
        const crcTable = new Uint32Array(256);
        for (let i = 0; i < 256; ++i) {
            let c = i;
            for (let k = 0; k < 8; ++k) c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
            crcTable[i] = c >>> 0;
        }
        function crc32(bytes) {
            let c = 0xffffffff;
            for (let i = 0; i < bytes.length; ++i) c = crcTable[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
            return (c ^ 0xffffffff) >>> 0;
        }

        const encoder = new TextEncoder();
        const localParts = [];
        const centralParts = [];
        let offset = 0;

        for (const item of fileEntries) {
            const nameBytes = encoder.encode(item.name);
            const data = item.data;
            const crc = crc32(data);

            const lfh = new Uint8Array(30 + nameBytes.length);
            const lv = new DataView(lfh.buffer);
            lv.setUint32(0, 0x04034b50, true);
            lv.setUint16(4, 20, true);
            lv.setUint16(6, 0x0800, true);
            lv.setUint16(8, 0, true);
            lv.setUint32(14, crc, true);
            lv.setUint32(18, data.length, true);
            lv.setUint32(22, data.length, true);
            lv.setUint16(26, nameBytes.length, true);
            lfh.set(nameBytes, 30);
            localParts.push(lfh, data);

            const cdh = new Uint8Array(46 + nameBytes.length);
            const cv = new DataView(cdh.buffer);
            cv.setUint32(0, 0x02014b50, true);
            cv.setUint16(4, 20, true);
            cv.setUint16(6, 20, true);
            cv.setUint16(8, 0x0800, true);
            cv.setUint16(10, 0, true);
            cv.setUint32(16, crc, true);
            cv.setUint32(20, data.length, true);
            cv.setUint32(24, data.length, true);
            cv.setUint16(28, nameBytes.length, true);
            cv.setUint32(42, offset, true);
            cdh.set(nameBytes, 46);
            centralParts.push(cdh);

            offset += lfh.length + data.length;
        }

        const cdSize = centralParts.reduce((s, p) => s + p.length, 0);
        const eocd = new Uint8Array(22);
        const ev = new DataView(eocd.buffer);
        ev.setUint32(0, 0x06054b50, true);
        ev.setUint16(8, fileEntries.length, true);
        ev.setUint16(10, fileEntries.length, true);
        ev.setUint32(12, cdSize, true);
        ev.setUint32(16, offset, true);

        return new Blob([...localParts, ...centralParts, eocd], { type: "application/zip" });
    }

    function collectMemfsFiles(dirPath, prefix, out) {
        let entries;
        try {
            entries = Module.FS.readdir(dirPath);
        } catch (_) {
            return;
        }
        for (const name of entries) {
            if (name === "." || name === "..") continue;
            const full = dirPath + "/" + name;
            const rel = prefix ? (prefix + "/" + name) : name;
            const st = Module.FS.stat(full);
            if (Module.FS.isDir(st.mode)) {
                collectMemfsFiles(full, rel, out);
            } else {
                out.push({ name: rel, data: Module.FS.readFile(full) });
            }
        }
    }

    // -----------------------------------------------------------------------
    // Lazy folder indexer (stores only File handles — zero RAM copy!)
    // -----------------------------------------------------------------------
    function buildLazyFolderIndex(filePairs, displayName) {
        // Locate best .exe and optional common wrapper prefix.
        let bestExe = "";
        let bestDepth = Infinity;
        let bestSize = -1;

        for (const pair of filePairs) {
            const norm = normalizeRelPath(pair.relPath);
            const lower = norm.toLowerCase();
            if (!lower.endsWith(".exe")) continue;
            const slash = lower.lastIndexOf("/");
            const leaf = slash >= 0 ? lower.slice(slash + 1) : lower;
            if (isIgnorableExe(leaf)) continue;
            const depth = norm.split("/").length - 1;
            const sz = pair.file ? pair.file.size : 0;
            if (depth < bestDepth || (depth === bestDepth && sz > bestSize)) {
                bestDepth = depth;
                bestSize = sz;
                bestExe = norm;
            }
        }

        let stripPrefix = "";
        let defaultExe = "";
        if (bestExe) {
            const slash = bestExe.lastIndexOf("/");
            if (slash >= 0) {
                stripPrefix = bestExe.slice(0, slash + 1);
                defaultExe = bestExe.slice(slash + 1);
            } else {
                defaultExe = bestExe;
            }
        }
        const stripPrefixLo = stripPrefix.toLowerCase();

        const files = new Map();
        const dirs = new Set([""]);
        const children = new Map();
        const seenChildren = new Map();

        function addChild(parentLo, name, isDir) {
            if (!children.has(parentLo)) children.set(parentLo, []);
            if (!seenChildren.has(parentLo)) seenChildren.set(parentLo, new Set());
            const nameLo = name.toLowerCase();
            if (!seenChildren.get(parentLo).has(nameLo)) {
                seenChildren.get(parentLo).add(nameLo);
                children.get(parentLo).push({ name, isDir });
            }
        }

        function ensureParents(relPath) {
            const parts = relPath.split("/");
            let cur = "";
            for (let i = 0; i + 1 < parts.length; ++i) {
                const parentLo = cur.toLowerCase();
                const seg = parts[i];
                cur = cur ? (cur + "/" + seg) : seg;
                const curLo = cur.toLowerCase();
                if (!dirs.has(curLo)) {
                    dirs.add(curLo);
                    addChild(parentLo, seg, true);
                }
            }
        }

        for (const pair of filePairs) {
            let rel = normalizeRelPath(pair.relPath);
            if (!rel) continue;
            if (stripPrefixLo) {
                if (!rel.toLowerCase().startsWith(stripPrefixLo)) continue;
                rel = rel.slice(stripPrefix.length);
            }
            if (!rel) continue;

            ensureParents(rel);
            const relLo = rel.toLowerCase();
            files.set(relLo, pair.file);

            const slash = rel.lastIndexOf("/");
            const parentLo = slash >= 0 ? rel.slice(0, slash).toLowerCase() : "";
            const leaf = slash >= 0 ? rel.slice(slash + 1) : rel;
            addChild(parentLo, leaf, false);
        }

        return {
            displayName: displayName || "web-folder",
            defaultExe,
            files,
            dirs,
            children,
        };
    }

    async function walkFileSystemEntry(entry, prefix, out) {
        if (entry.isFile) {
            await new Promise((resolve) => {
                entry.file(
                    (file) => {
                        out.push({ relPath: prefix ? (prefix + "/" + entry.name) : entry.name, file });
                        resolve();
                    },
                    () => resolve()
                );
            });
        } else if (entry.isDirectory) {
            const reader = entry.createReader();
            const nextPrefix = prefix ? (prefix + "/" + entry.name) : entry.name;
            while (true) {
                const batch = await new Promise((resolve) => {
                    reader.readEntries((entries) => resolve(entries || []), () => resolve([]));
                });
                if (!batch.length) break;
                for (const child of batch) {
                    await walkFileSystemEntry(child, nextPrefix, out);
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // Emscripten Module configuration & launcher hooks
    // -----------------------------------------------------------------------
    let runtimeReady = false;
    let touchState = 0; // 0=Auto, 1=On, 2=Off
    let muted = false;

    function setStatus(text) {
        const el = document.getElementById("status-text");
        if (el) el.textContent = text;
    }

    function setLauncherVisible(visible) {
        const overlay = document.getElementById("launcher-overlay");
        const stopBtn = document.getElementById("btn-stop");
        const editTouchBtn = document.getElementById("btn-touch-edit");
        if (overlay) overlay.classList.toggle("hidden", !visible);
        if (stopBtn) stopBtn.disabled = visible;
        if (editTouchBtn) editTouchBtn.disabled = visible;
    }

    async function launchWasmGame(mode, pathArg, gameKey) {
        if (!runtimeReady) {
            setStatus("Wait for WebAssembly initialization to finish...");
            return;
        }
        unlockTrackedAudio();
        setLauncherVisible(false);
        const rc = await Module.ccall(
            "gmrunner_web_launch",
            "number",
            ["string", "string", "string"],
            [mode, pathArg || "", gameKey],
            { async: true }
        );
        if (rc !== 0) {
            setLauncherVisible(true);
        } else {
            const canvas = document.getElementById("canvas");
            if (canvas) canvas.focus();
            if (touchState === 1) Module._gmrunner_web_set_touch_overlay(1);
            if (muted) Module._gmrunner_web_set_muted(1);
        }
    }

    async function handleFolderFileList(fileList) {
        if (!fileList || fileList.length === 0) return;
        setStatus(`Indexing ${fileList.length} files...`);
        const pairs = [];
        let rootName = "game";
        for (let i = 0; i < fileList.length; ++i) {
            const f = fileList[i];
            const rel = f.webkitRelativePath || f.name;
            if (i === 0 && rel.includes("/")) rootName = rel.split("/")[0];
            pairs.push({ relPath: rel, file: f });
        }
        const index = buildLazyFolderIndex(pairs, rootName);
        if (!index.defaultExe) {
            setStatus("No GameMaker .exe found in selected folder");
            return;
        }
        Module.gmWebZip = null;
        Module.gmWebFs = index;
        await launchWasmGame("folder", index.defaultExe, rootName);
    }

    async function handleSingleFile(file) {
        if (!file) return;
        const lower = file.name.toLowerCase();
        const key = file.name.replace(/\.[^.]+$/, "");
        if (lower.endsWith(".zip")) {
            Module.gmWebFs = null;
            Module.gmWebZip = { file };
            await launchWasmGame("zip", "", key);
        } else if (lower.endsWith(".exe")) {
            setStatus(`Loading ${file.name}...`);
            const bytes = new Uint8Array(await file.arrayBuffer());
            ensureMemfsDir("/game");
            const memPath = "/game/" + file.name;
            Module.FS.writeFile(memPath, bytes);
            Module.gmWebFs = null;
            Module.gmWebZip = null;
            await launchWasmGame("memfs", memPath, key);
        } else {
            setStatus("Unsupported file type — choose a folder, .zip, or .exe");
        }
    }

    async function tryAutoloadFromQuery() {
        const params = new URLSearchParams(window.location.search);
        const gameUrl = params.get("game");
        if (!gameUrl) return;
        try {
            setStatus(`Fetching ${gameUrl}...`);
            const resp = await fetch(gameUrl);
            if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
            const blob = await resp.blob();
            const filename = decodeURIComponent(gameUrl.split("/").pop().split("?")[0] || "game.zip");
            const file = new File([blob], filename);
            await handleSingleFile(file);
        } catch (err) {
            console.error("[gmrunner] Failed to fetch ?game URL:", err);
            setStatus("Failed to load URL: " + err.message);
        }
    }

    window.Module = {
        arguments: new URLSearchParams(window.location.search).has("sharp-scale") ? ["--sharp-scale"] : [],
        canvas: (function () {
            return document.getElementById("canvas");
        })(),
        gmWebFs: null,
        gmWebZip: null,
        gmMountPersist: mountPersist,
        gmSchedulePersistSync: schedulePersistSync,
        gmFlushPersistSync: flushPersistNow,
        gmUnlockAudio: unlockTrackedAudio,
        gmSetStatus: setStatus,
        print: (text) => console.log("[gmrunner]", text),
        printErr: (text) => console.warn("[gmrunner]", text),
        onRuntimeInitialized: function () {
            runtimeReady = true;
            tryAutoloadFromQuery();
        },
    };

    // -----------------------------------------------------------------------
    // UI event bindings
    // -----------------------------------------------------------------------
    window.addEventListener("DOMContentLoaded", () => {
        const folderInput = document.getElementById("input-folder");
        const fileInput = document.getElementById("input-file");
        const dropZone = document.getElementById("drop-zone");
        const canvas = Module.canvas;

        // SDL handles canvas touches via pointer events. Let Chrome skip its
        // default tap handling, which can delay game frames while a finger is down.
        canvas.addEventListener("touchstart", (ev) => {
            if (ev.cancelable) ev.preventDefault();
            if (document.activeElement !== canvas) canvas.focus({ preventScroll: true });
        }, { passive: false });

        const openFolder = () => folderInput && folderInput.click();
        const openFile = () => fileInput && fileInput.click();

        document.getElementById("btn-pick-folder")?.addEventListener("click", openFolder);
        document.getElementById("hero-pick-folder")?.addEventListener("click", openFolder);
        document.getElementById("btn-pick-file")?.addEventListener("click", openFile);
        document.getElementById("hero-pick-file")?.addEventListener("click", openFile);

        folderInput?.addEventListener("change", (ev) => {
            handleFolderFileList(ev.target.files);
            ev.target.value = "";
        });

        fileInput?.addEventListener("change", (ev) => {
            if (ev.target.files && ev.target.files[0]) {
                handleSingleFile(ev.target.files[0]);
            }
            ev.target.value = "";
        });

        document.getElementById("btn-stop")?.addEventListener("click", async () => {
            if (!runtimeReady) return;
            await Module.ccall("gmrunner_web_stop", null, [], [], { async: true });
            setLauncherVisible(true);
        });

        document.getElementById("btn-touch")?.addEventListener("click", (ev) => {
            touchState = (touchState + 1) % 3;
            const labels = ["Touch: Auto", "Touch: On", "Touch: Off"];
            ev.target.textContent = labels[touchState];
            if (runtimeReady && touchState !== 0) {
                Module._gmrunner_web_set_touch_overlay(touchState === 1 ? 1 : 0);
            }
        });

        document.getElementById("btn-touch-edit")?.addEventListener("click", () => {
            if (!runtimeReady || !Module._gmrunner_web_set_touch_edit_mode) return;
            const isEditing = Module._gmrunner_web_is_touch_editing && Module._gmrunner_web_is_touch_editing() !== 0;
            if (!isEditing && touchState === 2) {
                touchState = 1;
                const touchBtn = document.getElementById("btn-touch");
                if (touchBtn) touchBtn.textContent = "Touch: On";
            }
            Module._gmrunner_web_set_touch_edit_mode(isEditing ? 0 : 1);
            if (canvas) canvas.focus();
        });

        document.getElementById("btn-mute")?.addEventListener("click", (ev) => {
            muted = !muted;
            ev.target.textContent = muted ? "Audio: Muted" : "Audio: On";
            if (runtimeReady) {
                Module._gmrunner_web_set_muted(muted ? 1 : 0);
            }
        });

        document.getElementById("btn-fullscreen")?.addEventListener("click", () => {
            if (!document.fullscreenElement) {
                (canvas?.requestFullscreen || dropZone?.requestFullscreen)?.call(canvas || dropZone);
            } else {
                document.exitFullscreen?.();
            }
        });

        document.getElementById("btn-export-saves")?.addEventListener("click", () => {
            if (!runtimeReady || !Module.FS) return;
            const out = [];
            collectMemfsFiles("/persist/" + activeGameKey, "", out);
            if (out.length === 0) {
                setStatus("No save files to export yet");
                return;
            }
            const blob = buildStoreZip(out);
            const url = URL.createObjectURL(blob);
            const a = document.createElement("a");
            a.href = url;
            a.download = `gmrunner_saves_${activeGameKey}.zip`;
            a.click();
            setTimeout(() => URL.revokeObjectURL(url), 5000);
        });

        // Drag-and-drop handler supporting folders, .zip archives, and .exe files.
        if (dropZone) {
            dropZone.addEventListener("dragover", (ev) => {
                ev.preventDefault();
                dropZone.classList.add("drag-over");
            });
            dropZone.addEventListener("dragleave", () => {
                dropZone.classList.remove("drag-over");
            });
            dropZone.addEventListener("drop", async (ev) => {
                ev.preventDefault();
                dropZone.classList.remove("drag-over");
                const items = ev.dataTransfer?.items;
                if (items && items.length > 0 && typeof items[0].webkitGetAsEntry === "function") {
                    const firstEntry = items[0].webkitGetAsEntry();
                    if (firstEntry && firstEntry.isDirectory) {
                        setStatus(`Scanning folder "${firstEntry.name}"...`);
                        const pairs = [];
                        await walkFileSystemEntry(firstEntry, "", pairs);
                        const index = buildLazyFolderIndex(pairs, firstEntry.name);
                        if (!index.defaultExe) {
                            setStatus("No GameMaker .exe found in dropped folder");
                            return;
                        }
                        Module.gmWebZip = null;
                        Module.gmWebFs = index;
                        await launchWasmGame("folder", index.defaultExe, firstEntry.name);
                        return;
                    }
                }
                const files = ev.dataTransfer?.files;
                if (files && files.length === 1) {
                    await handleSingleFile(files[0]);
                } else if (files && files.length > 1) {
                    await handleFolderFileList(files);
                }
            });
        }

        // Resume WebAudio on any early user gesture & flush persistent saves on tab hide/exit.
        window.addEventListener("pointerdown", unlockTrackedAudio, { passive: true });
        window.addEventListener("keydown", unlockTrackedAudio, { passive: true });
        document.addEventListener("visibilitychange", () => {
            if (document.visibilityState === "hidden") flushPersistNow();
        });
        window.addEventListener("pagehide", () => {
            flushPersistNow();
        });
    });
})();
