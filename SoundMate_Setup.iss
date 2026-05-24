; 이 파일은 Inno Setup GUI 컴파일러에서 직접 열어 빌드할 수 있는 최신 설정 파일입니다.
; 파워쉘 스크립트(.ps1) 대신 C++ 전용 setup/uninstall 바이너리를 안전하게 실행합니다.

[Setup]
AppName=SoundMate Equalizer
AppVersion=0.0.1
DefaultDirName={pf}\SoundMate Equalizer
DefaultGroupName=SoundMate
LicenseFile=Agreement.txt
PrivilegesRequired=admin
OutputBaseFilename=SoundMate_Setup_v0.0.1
OutputDir=installer_output
Compression=lzma
SolidCompression=yes
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64

[Languages]
Name: "korean"; MessagesFile: "Korean.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; 메인 실행 파일 및 APO DLL
Source: "build\Release\SoundMate Equalizer.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\Release\SoundMate_APO.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\Release\SoundMate_Controller.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\Release\SoundMate_reset.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\Release\SoundMate_uninstall.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\Release\SoundMate_setup.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\Release\libcurl-x64.dll"; DestDir: "{app}"; Flags: ignoreversion


; config 및 기타 필요한 리소스들
Source: "config\*"; DestDir: "{app}\config"; Flags: ignoreversion recursesubdirs createallsubdirs

; 앨범 placeholder 등 assets
Source: "build\Release\assets\*"; DestDir: "{app}\assets"; Flags: ignoreversion recursesubdirs createallsubdirs

; 로그 수집 도구
Source: "tool\*"; DestDir: "{app}\tool"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\SoundMate Equalizer"; Filename: "{app}\SoundMate Equalizer.exe"
Name: "{commondesktop}\SoundMate Equalizer"; Filename: "{app}\SoundMate Equalizer.exe"; Tasks: desktopicon

; 덮어쓰기 설치 전 옛 Controller/EQ를 강제 종료해야 새 APO/DLL 로딩이 정상 동작.
[InstallDelete]
Type: filesandordirs; Name: "{app}\logs"

[Run]
; 설치 직전 옛 프로세스 정리
Filename: "{cmd}"; Parameters: "/C taskkill /F /IM SoundMate_Controller.exe /IM ""SoundMate Equalizer.exe"""; Flags: runhidden waituntilterminated; StatusMsg: "이전 인스턴스 종료 중..."
; 설치 완료 후 C++ 드라이버 자동 등록 프로그램 실행 (PowerShell 의존성 완전 제거)
Filename: "{app}\SoundMate_setup.exe"; Flags: waituntilterminated runhidden; StatusMsg: "사운드메이트 오디오 드라이버 설정 및 레지스트리 등록 중..."
; 설치 완료 후 프로그램 자동 실행
Filename: "{app}\SoundMate Equalizer.exe"; Description: "{cm:LaunchProgram,SoundMate Equalizer}"; Flags: nowait postinstall skipifsilent

; PowerShell 의존 제거. C++ uninstall이 모든 청소 담당.
[UninstallRun]
Filename: "{app}\SoundMate_uninstall.exe"; Flags: runhidden waituntilterminated
