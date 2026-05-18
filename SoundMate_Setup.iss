[Setup]
AppName=SoundMate Equalizer
AppVersion=1.0.0
DefaultDirName={pf}\SoundMate Equalizer
DefaultGroupName=SoundMate
OutputBaseFilename=SoundMate_Setup_v1.0.0
OutputDir=installer_output
Compression=lzma
SolidCompression=yes
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; 메인 실행 파일 및 APO DLL
Source: "build\Release\SoundMate_EQ.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\Release\SoundMate_APO.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\Release\SoundMate_Controller.exe"; DestDir: "{app}"; Flags: ignoreversion

; 레지스트리 설정 스크립트 등
Source: "reset_registry.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "registry_cleanup.ps1"; DestDir: "{app}"; Flags: ignoreversion

; config 및 기타 필요한 리소스들
Source: "config\*"; DestDir: "{app}\config"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\SoundMate Equalizer"; Filename: "{app}\SoundMate_EQ.exe"
Name: "{commondesktop}\SoundMate Equalizer"; Filename: "{app}\SoundMate_EQ.exe"; Tasks: desktopicon

[Run]
; 설치 완료 후 레지스트리 자동 등록 스크립트 백그라운드 실행
Filename: "powershell.exe"; Parameters: "-ExecutionPolicy Bypass -WindowStyle Hidden -File ""{app}\reset_registry.ps1"""; Flags: waituntilterminated runhidden
; 설치 완료 후 프로그램 자동 실행
Filename: "{app}\SoundMate_EQ.exe"; Description: "{cm:LaunchProgram,SoundMate Equalizer}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; 언인스톨 전 APO 레지스트리 클린업 스크립트 실행
Filename: "powershell.exe"; Parameters: "-ExecutionPolicy Bypass -WindowStyle Hidden -File ""{app}\registry_cleanup.ps1"""; Flags: waituntilterminated runhidden
