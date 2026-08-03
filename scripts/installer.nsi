; Windows installer for a packaged Substrate game.
;
; Driven by scripts/release_windows.sh, which passes GAME, VERSION, STAGE and OUTFILE with
; -D. Nothing here reads the source tree: everything installed comes out of STAGE, which
; scripts/manifest.py decided the contents of.
;
; ## Why this installs per-user
;
; The default install directory is $LOCALAPPDATA\Programs\<game>, not $PROGRAMFILES, and
; that is a deliberate consequence of how the engine resolves paths rather than a
; preference about UAC.
;
; Substrate reads its assets relative to the executable -- that is what SUBSTRATE_PORTABLE
; does -- but *writes* its log, its Chrome trace and its screenshots relative to the working
; directory. Writes were left alone on purpose: scripts/golden.sh, scripts/baseline.py and
; scripts/rdoc.sh all agree that debug_frames/ is at the repo root, and moving them would
; have been a regression in every one of those.
;
; A per-user install directory is therefore writable, the shortcut can start the game in it,
; and the read path and the write path need no second policy between them. Installing into
; Program Files would mean the write path had to learn about SHGetKnownFolderPath, which is
; a change to the engine to satisfy the installer. This is the same choice VS Code makes,
; and it also means no elevation prompt.

Unicode true
SetCompressor /SOLID lzma

!define APPNAME "${GAME}"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Substrate-${GAME}"

Name "${APPNAME} ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\${APPNAME}"
InstallDirRegKey HKCU "Software\Substrate\${APPNAME}" "InstallDir"

; Per-user throughout. `user` rather than `admin` keeps Windows from showing an elevation
; prompt for an install that writes nothing outside the user's own profile.
RequestExecutionLevel user
ShowInstDetails show
ShowUnInstDetails show

!include "MUI2.nsh"

; Before any use of ${GetSize}, which is what supplies the Add/Remove Programs size. NSIS
; resolves these at parse time, so an include at the bottom of the file is not an include
; at all -- it fails with `Invalid command: "${GetSize}"`, naming the symbol and not the
; ordering.
!include "FileFunc.nsh"
!insertmacro GetSize

!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\${GAME}.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Run ${APPNAME}"

!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Section "Install"
    SetOutPath "$INSTDIR"

    ; The whole staged tree, recursively, and its shape is load-bearing: shaders/ and
    ; shaders/game/ beside the exe, and the two asset trees at engine/assets and
    ; game/<name>/assets, the same distance apart as in the source tree. A glTF names its
    ; buffers and images relative to itself and the composite scenes reach across into the
    ; other tree, so flattening this here would break them exactly as it broke the AppImage.
    File /r "${STAGE}\*.*"

    WriteRegStr HKCU "Software\Substrate\${APPNAME}" "InstallDir" "$INSTDIR"

    ; Shown in Add/Remove Programs. EstimatedSize is in KB and is what stops the entry
    ; reading as 0 bytes for a package that is mostly textures.
    WriteRegStr   HKCU "${UNINST_KEY}" "DisplayName"     "${APPNAME}"
    WriteRegStr   HKCU "${UNINST_KEY}" "DisplayVersion"  "${VERSION}"
    WriteRegStr   HKCU "${UNINST_KEY}" "Publisher"       "Substrate"
    WriteRegStr   HKCU "${UNINST_KEY}" "UninstallString" "$\"$INSTDIR\uninstall.exe$\""
    WriteRegStr   HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
    WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1

    ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
    IntFmt $0 "0x%08X" $0
    WriteRegDWORD HKCU "${UNINST_KEY}" "EstimatedSize" "$0"

    ; SetOutPath before CreateShortcut is what sets the shortcut's "Start in" directory,
    ; and it is the reason a per-user install works: the game runs with its own directory
    ; as the working directory, so the log and the trace land beside it instead of in
    ; whatever directory Explorer happened to hand over.
    SetOutPath "$INSTDIR"
    CreateShortcut "$SMPROGRAMS\${APPNAME}.lnk" "$INSTDIR\${GAME}.exe" "" "$INSTDIR\${GAME}.exe" 0

    WriteUninstaller "$INSTDIR\uninstall.exe"
SectionEnd

Section "Uninstall"
    Delete "$SMPROGRAMS\${APPNAME}.lnk"

    ; RMDir /r on $INSTDIR, which is only safe because the install is per-user and the
    ; directory was created by this installer. debug_frames/ is removed with it, and that
    ; is intended -- it holds logs and traces this game wrote, not the user's documents.
    RMDir /r "$INSTDIR"

    DeleteRegKey HKCU "${UNINST_KEY}"
    DeleteRegKey HKCU "Software\Substrate\${APPNAME}"
SectionEnd
