; SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
; SPDX-License-Identifier: GPL-3.0-or-later

Unicode true
SetCompressor /SOLID lzma
RequestExecutionLevel admin

!include "MUI2.nsh"
!include "StrFunc.nsh"
!include "x64.nsh"
${StrStr}

!ifndef Stage
  !error "Stage must point to the deployed Windows application tree."
!endif
!ifndef AppVersion
  !error "AppVersion must contain the release version."
!endif
!ifndef AssetsDirectory
  !error "AssetsDirectory must point to the viewer icon directory."
!endif
!ifndef LicenseFile
  !error "LicenseFile must point to the viewer license."
!endif
!ifndef InstallerOutput
  !error "InstallerOutput must contain the output executable path."
!endif

!define ProductName "P2000M VID2VGA Viewer"
!define ProductId "nl.ivofilot.p2000m.vid2vga.viewer"
!define ProductRegistryKey "Software\Ivo Filot\P2000M VID2VGA Viewer"
!define UninstallRegistryKey "Software\Microsoft\Windows\CurrentVersion\Uninstall\${ProductId}"

Name "${ProductName} ${AppVersion}"
OutFile "${InstallerOutput}"
InstallDir "$PROGRAMFILES64\P2000M VID2VGA Viewer"
InstallDirRegKey HKLM "${ProductRegistryKey}" "InstallLocation"
BrandingText "P2000M VID2VGA Viewer"
Icon "${AssetsDirectory}\p2000m-vid2vga-viewer.ico"
UninstallIcon "${AssetsDirectory}\p2000m-vid2vga-viewer.ico"
VIProductVersion "${AppVersion}.0"
VIAddVersionKey /LANG=1033 "ProductName" "${ProductName}"
VIAddVersionKey /LANG=1033 "ProductVersion" "${AppVersion}"
VIAddVersionKey /LANG=1033 "FileVersion" "${AppVersion}"
VIAddVersionKey /LANG=1033 "CompanyName" "Ivo Filot"
VIAddVersionKey /LANG=1033 "FileDescription" "${ProductName} Setup"
VIAddVersionKey /LANG=1033 "LegalCopyright" "Copyright 2026 Ivo Filot"

!define MUI_ABORTWARNING
!define MUI_ICON "${AssetsDirectory}\p2000m-vid2vga-viewer.ico"
!define MUI_UNICON "${AssetsDirectory}\p2000m-vid2vga-viewer.ico"
!define MUI_FINISHPAGE_RUN "$INSTDIR\p2000m-vid2vga-viewer.exe"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${LicenseFile}"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Var LegacyUninstallKey

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "${ProductName} requires 64-bit Windows."
    Abort
  ${EndIf}

  SetRegView 64
  SetShellVarContext all

  ; Qt IFW generated a random uninstall key for every installation. Find the
  ; old entry by its exact display name, reuse its directory, and remember the
  ; key so the first NSIS installation can remove the broken registration.
  StrCpy $0 0
legacy_loop:
  EnumRegKey $1 HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall" $0
  StrCmp $1 "" legacy_done
  StrCmp $1 "${ProductId}" legacy_next
  ReadRegStr $2 HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\$1" "DisplayName"
  StrCmp $2 "${ProductName}" 0 legacy_next
  ReadRegStr $3 HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\$1" "UninstallString"
  ${StrStr} $4 $3 "p2000m-vid2vga-maintenance"
  StrCmp $4 "" legacy_next
  ReadRegStr $3 HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\$1" "InstallLocation"
  StrCmp $3 "" +2
    StrCpy $INSTDIR $3
  StrCpy $LegacyUninstallKey $1
  Goto legacy_done
legacy_next:
  IntOp $0 $0 + 1
  Goto legacy_loop
legacy_done:
FunctionEnd

Section "Install"
  SetRegView 64
  SetShellVarContext all
  SetOutPath "$INSTDIR"

  File /r "${Stage}\*"
  WriteUninstaller "$INSTDIR\uninstall.exe"

  ; The former Qt installer created per-user shortcuts even though it installed
  ; under Program Files. Remove those before creating system-wide replacements.
  SetShellVarContext current
  Delete "$DESKTOP\P2000M VID2VGA Viewer.lnk"
  Delete "$SMPROGRAMS\P2000M VID2VGA Viewer\P2000M VID2VGA Viewer.lnk"
  RMDir "$SMPROGRAMS\P2000M VID2VGA Viewer"
  SetShellVarContext all
  CreateDirectory "$SMPROGRAMS\P2000M VID2VGA Viewer"
  CreateShortcut "$SMPROGRAMS\P2000M VID2VGA Viewer\P2000M VID2VGA Viewer.lnk" "$INSTDIR\p2000m-vid2vga-viewer.exe" "" "$INSTDIR\p2000m-vid2vga-viewer.exe"
  CreateShortcut "$DESKTOP\P2000M VID2VGA Viewer.lnk" "$INSTDIR\p2000m-vid2vga-viewer.exe" "" "$INSTDIR\p2000m-vid2vga-viewer.exe"

  WriteRegStr HKLM "${ProductRegistryKey}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UninstallRegistryKey}" "DisplayName" "${ProductName}"
  WriteRegStr HKLM "${UninstallRegistryKey}" "DisplayVersion" "${AppVersion}"
  WriteRegStr HKLM "${UninstallRegistryKey}" "Publisher" "Ivo Filot"
  WriteRegStr HKLM "${UninstallRegistryKey}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UninstallRegistryKey}" "DisplayIcon" "$INSTDIR\p2000m-vid2vga-viewer.exe"
  WriteRegStr HKLM "${UninstallRegistryKey}" "UninstallString" '$\"$INSTDIR\uninstall.exe$\"'
  WriteRegStr HKLM "${UninstallRegistryKey}" "QuietUninstallString" '$\"$INSTDIR\uninstall.exe$\" /S'
  WriteRegStr HKLM "${UninstallRegistryKey}" "URLInfoAbout" "https://github.com/ifilot/p2000m-video-to-vga-adapter"
  WriteRegDWORD HKLM "${UninstallRegistryKey}" "NoModify" 1
  WriteRegDWORD HKLM "${UninstallRegistryKey}" "NoRepair" 1

  ; Remove metadata left by the Qt IFW 0.3.0/0.3.1 installer only after the
  ; replacement application and its working uninstaller have been written.
  Delete "$INSTDIR\p2000m-vid2vga-maintenance.exe"
  Delete "$INSTDIR\p2000m-vid2vga-maintenance.dat"
  Delete "$INSTDIR\p2000m-vid2vga-maintenance.ini"
  Delete "$INSTDIR\components.xml"
  Delete "$INSTDIR\installer.dat"
  Delete "$INSTDIR\network.xml"
  Delete "$INSTDIR\InstallationLog.txt"
  StrCmp $LegacyUninstallKey "" +2
    DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\$LegacyUninstallKey"
SectionEnd

Section "Uninstall"
  SetRegView 64
  SetShellVarContext current
  Delete "$DESKTOP\P2000M VID2VGA Viewer.lnk"
  RMDir /r "$SMPROGRAMS\P2000M VID2VGA Viewer"
  SetShellVarContext all
  Delete "$DESKTOP\P2000M VID2VGA Viewer.lnk"
  RMDir /r "$SMPROGRAMS\P2000M VID2VGA Viewer"
  DeleteRegKey HKLM "${UninstallRegistryKey}"
  DeleteRegKey HKLM "${ProductRegistryKey}"
  RMDir /r "$INSTDIR"
SectionEnd
