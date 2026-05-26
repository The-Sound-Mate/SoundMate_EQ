; ============================================================================
; SoundMate_Setup.iss — v3.2 Final (Revised) Master Blueprint
; 이 파일은 Inno Setup GUI 컴파일러에서 직접 열어 빌드할 수 있는 최신 설정 파일입니다.
;
; [설계 원칙]
;   1. 샌드박스 스테이징 배포: 바이너리를 {app}\_tmp_install\ 에 1차 복사하여
;      audiodg.exe 파일 잠금으로 인한 [Files] 복사 실패를 원천 차단.
;   2. 원자적 치환 + Graceful Degradation: ReplaceFileW 실패 시 PFRO 격하.
;   3. Zero-Pollution 롤백: 스테이징 폴더 삭제로 PFRO 무효화.
;   4. 이중 가드: SystemModified + InstallSucceeded / UninstallSucceeded.
; ============================================================================

[Setup]
AppName=SoundMate Equalizer
AppVersion=0.0.2
DefaultDirName={commonpf}\SoundMate Equalizer
DefaultGroupName=SoundMate Equalizer
LicenseFile=Agreement.txt
PrivilegesRequired=admin
OutputBaseFilename=SoundMate_Setup_v0.0.2
OutputDir=installer_output
SetupIconFile=SoundMate_ImGui\assets\logo.ico
Compression=lzma
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

; [APO Lock] 실행 중인 인스턴스 자동 감지 + WM_CLOSE 정상 종료
AppMutex=Global\SoundMate_EQ_AppMutex
CloseApplications=force
RestartApplications=no

; DisableDirPage=no — 사용자가 설치 드라이브를 선택할 수 있되,
; NextButtonClick 에서 시스템/루트 폴더를 차단하여 안전성 확보.
DisableDirPage=no

[Languages]
Name: "korean"; MessagesFile: "Korean.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

; ============================================================================
; [Files] — 샌드박스 스테이징 배포
; ============================================================================
[Files]
; --- 잠금 위험 파일: 스테이징 경로로 1차 복사 ---
Source: "build\Release\SoundMate_APO.dll"; DestDir: "{app}\_tmp_install"; Flags: ignoreversion

; --- 잠금 위험 없는 파일: {app} 에 직접 복사 ---
Source: "build\Release\SoundMate Equalizer.exe"; DestDir: "{app}"; Flags: ignoreversion
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

[InstallDelete]
Type: filesandordirs; Name: "{app}\logs"

; ============================================================================
; [Run] — SoundMate_setup.exe 호출은 [Code] ssPostInstall 로 이동됨.
; ============================================================================
[Run]
Filename: "{app}\SoundMate Equalizer.exe"; Description: "{cm:LaunchProgram,SoundMate Equalizer}"; Flags: nowait postinstall skipifsilent shellexec

[UninstallRun]
Filename: "{app}\SoundMate_uninstall.exe"; RunOnceId: "UninstallDriver"; Flags: runhidden waituntilterminated

; ============================================================================
; [Code] — v3.2 Final (Revised) 블루프린트 완전 구현
; ============================================================================
[Code]
const
  MOVEFILE_DELAY_UNTIL_REBOOT = $00000004;
  MOVEFILE_REPLACE_EXISTING   = $00000001;

// --- Win32 API 외부 함수 선언 ---
function MoveFileExW(lpExistingFileName, lpNewFileName: String; dwFlags: Cardinal): Boolean;
external 'MoveFileExW@kernel32.dll stdcall';

function MoveFileExDelete(lpExistingFileName: String; lpNewFileName: Longint; dwFlags: Cardinal): Boolean;
external 'MoveFileExW@kernel32.dll stdcall';

function ReplaceFileW(
  lpReplacedFileName, lpReplacementFileName, lpBackupFileName: String;
  dwReplaceFlags: Cardinal;
  lpExclude, lpReserved: Longint
): Boolean;
external 'ReplaceFileW@kernel32.dll stdcall';

// --- 전역 변수 ---
var
  NeedsReboot: Boolean;
  InstallSucceeded: Boolean;
  SystemModified: Boolean;

  UninstallNeedsReboot: Boolean;
  UninstallSucceeded: Boolean;
  UninstallSystemModified: Boolean;

  OrigPPLExists: Boolean;
  OrigPPLValue: Cardinal;

// ============================================================================
// [공통] 설치 경로 검증
// ============================================================================
function NextButtonClick(CurPageID: Integer): Boolean;
var
  Dir, DirLower: String;
begin
  Result := True;
  if CurPageID = wpSelectDir then begin
    Dir := RemoveBackslashUnlessRoot(WizardDirValue);
    DirLower := Lowercase(Dir);

    if (Length(DirLower) <= 3) and (DirLower[2] = ':') then begin
      MsgBox('드라이브 루트에는 직접 설치할 수 없습니다.' + #13#10 +
             '하위 폴더를 지정해 주세요.', mbError, MB_OK);
      Result := False;
      Exit;
    end;

    if (CompareText(Dir, ExpandConstant('{sys}')) = 0) or
       (CompareText(Dir, ExpandConstant('{commonpf}')) = 0) or
       (CompareText(Dir, ExpandConstant('{commonpf32}')) = 0) or
       (CompareText(Dir, ExpandConstant('{commoncf}')) = 0) or
       (CompareText(Dir, ExpandConstant('{win}')) = 0) then begin
      MsgBox('시스템 핵심 폴더에는 설치할 수 없습니다.' + #13#10 +
             'SoundMate EQ의 안전한 동작을 위해 전용 하위 폴더를 지정해 주세요.',
             mbError, MB_OK);
      Result := False;
    end;
  end;
end;

// ============================================================================
// PPL 백업/복원 헬퍼
// ============================================================================
procedure BackupPPL;
var
  Val: Cardinal;
begin
  if RegQueryDWordValue(HKLM, 'SYSTEM\CurrentControlSet\Control\Audio',
                        'DisableProtectedAudioDG', Val) then begin
    OrigPPLExists := True;
    OrigPPLValue := Val;
  end else begin
    OrigPPLExists := False;
    OrigPPLValue := 0;
  end;
end;

procedure RestorePPL;
begin
  if OrigPPLExists then
    RegWriteDWordValue(HKLM, 'SYSTEM\CurrentControlSet\Control\Audio',
                       'DisableProtectedAudioDG', OrigPPLValue)
  else
    RegDeleteValue(HKLM, 'SYSTEM\CurrentControlSet\Control\Audio',
                   'DisableProtectedAudioDG');
end;

// ============================================================================
// [설치] DeployFile — 개별 파일 배포 루틴
// ============================================================================
procedure DeployFile(const FileName: String);
var
  StagingPath, TargetPath, BackupPath: String;
begin
  StagingPath := ExpandConstant('{app}\_tmp_install\' + FileName);
  TargetPath := ExpandConstant('{app}\' + FileName);
  BackupPath := TargetPath + '.bak';

  if not FileExists(StagingPath) then begin
    Log('[DeployFile] Staging file not found, skip: ' + FileName);
    Exit;
  end;

  if NeedsReboot then begin
    MoveFileExW(StagingPath, TargetPath,
                MOVEFILE_DELAY_UNTIL_REBOOT or MOVEFILE_REPLACE_EXISTING);
    Log('[DeployFile] PFRO scheduled (NeedsReboot was set): ' + FileName);
  end

  else if FileExists(TargetPath) then begin
    if ReplaceFileW(TargetPath, StagingPath, BackupPath, 0, 0, 0) then begin
      DeleteFile(BackupPath);
      Log('[DeployFile] ReplaceFileW succeeded: ' + FileName);
    end else begin
      MoveFileExW(StagingPath, TargetPath,
                  MOVEFILE_DELAY_UNTIL_REBOOT or MOVEFILE_REPLACE_EXISTING);
      NeedsReboot := True;
      Log('[DeployFile] ReplaceFileW failed, fallback to PFRO: ' + FileName);
    end;
  end

  else begin
    if RenameFile(StagingPath, TargetPath) then
      Log('[DeployFile] RenameFile succeeded (fresh install): ' + FileName)
    else begin
      MoveFileExW(StagingPath, TargetPath,
                  MOVEFILE_DELAY_UNTIL_REBOOT or MOVEFILE_REPLACE_EXISTING);
      NeedsReboot := True;
      Log('[DeployFile] RenameFile failed, fallback to PFRO: ' + FileName);
    end;
  end;
end;

// ============================================================================
// [설치] PrepareToInstall
// ============================================================================
function PrepareToInstall(var NeedsRestartParam: Boolean): String;
var
  Rc: Integer;
  Retry: Integer;
begin
  Result := '';
  NeedsReboot := False;
  InstallSucceeded := False;
  SystemModified := False;

  if DirExists(ExpandConstant('{app}\_tmp_install')) then begin
    DelTree(ExpandConstant('{app}\_tmp_install'), True, True, True);
    Log('[PrepareToInstall] Cleaned residual _tmp_install folder.');
  end;

  Exec(ExpandConstant('{cmd}'),
       '/C taskkill /F /IM "SoundMate Equalizer.exe" /IM SoundMate_Controller.exe /IM MainController.exe',
       '', SW_HIDE, ewWaitUntilTerminated, Rc);

  BackupPPL;
  RegWriteDWordValue(HKLM, 'SYSTEM\CurrentControlSet\Control\Audio',
                     'DisableProtectedAudioDG', 1);
  SystemModified := True;

  WizardForm.PreparingLabel.Caption :=
    '오디오 서비스를 일시 중지하고 파일을 교체합니다 (5~30초간 음원이 일시 정지될 수 있습니다)';
  Exec(ExpandConstant('{cmd}'),
       '/C net stop audiosrv /y',
       '', SW_HIDE, ewWaitUntilTerminated, Rc);

  for Retry := 1 to 15 do begin
    if Exec(ExpandConstant('{cmd}'),
            '/C taskkill /F /IM audiodg.exe',
            '', SW_HIDE, ewWaitUntilTerminated, Rc) then begin
      if Rc = 0 then begin
        Log('[PrepareToInstall] audiodg.exe killed at retry #' + IntToStr(Retry));
        Break;
      end;
    end;
    Sleep(200);
  end;

  Sleep(500);
end;

// ============================================================================
// [설치] ssPostInstall
// ============================================================================
procedure CurStepChanged(CurStep: TSetupStep);
var
  Rc: Integer;
begin
  if CurStep = ssPostInstall then begin
    DeployFile('SoundMate_APO.dll');

    if not NeedsReboot then begin
      if FileExists(ExpandConstant('{app}\SoundMate_setup.exe')) then begin
        Exec(ExpandConstant('{app}\SoundMate_setup.exe'), '',
             '', SW_HIDE, ewWaitUntilTerminated, Rc);
        if Rc = 0 then begin
          Log('[ssPostInstall] SoundMate_setup.exe succeeded.');
          InstallSucceeded := True;
        end else begin
          Log('[ssPostInstall] SoundMate_setup.exe failed: ' + IntToStr(Rc));
          InstallSucceeded := False;
        end;
      end;
      DelTree(ExpandConstant('{app}\_tmp_install'), True, True, True);
    end else begin
      if FileExists(ExpandConstant('{app}\SoundMate_setup.exe')) then begin
        Exec(ExpandConstant('{app}\SoundMate_setup.exe'), '',
             '', SW_HIDE, ewWaitUntilTerminated, Rc);
        Log('[ssPostInstall] SoundMate_setup.exe ran in PFRO mode.');
      end;
      MoveFileExDelete(ExpandConstant('{app}\_tmp_install'), 0,
                       MOVEFILE_DELAY_UNTIL_REBOOT);
      Log('[ssPostInstall] _tmp_install PFRO delete scheduled.');
      InstallSucceeded := True;
    end;
  end;
end;

// ============================================================================
// [설치] NeedRestart — 재부팅 프롬프트 이중 방어
// ============================================================================
function NeedRestart(): Boolean;
begin
  Result := NeedsReboot;
end;

// ============================================================================
// [설치] DeinitializeSetup — 안전 롤백
// ============================================================================
procedure DeinitializeSetup();
var
  Rc: Integer;
begin
  if (not InstallSucceeded) and SystemModified then begin
    Log('[DeinitializeSetup] Rollback triggered.');
    DelTree(ExpandConstant('{app}\_tmp_install'), True, True, True);
    Exec(ExpandConstant('{cmd}'),
         '/C net start AudioEndpointBuilder & net start audiosrv',
         '', SW_HIDE, ewWaitUntilTerminated, Rc);
    RestorePPL;
    Log('[DeinitializeSetup] Rollback complete.');
  end;
end;

// ============================================================================
// ================== 제거 (Uninstaller) ====================
// ============================================================================

procedure CleanUpFile(const Path: String);
begin
  if FileExists(Path) then begin
    if not DeleteFile(Path) then begin
      MoveFileExDelete(Path, 0, MOVEFILE_DELAY_UNTIL_REBOOT);
      UninstallNeedsReboot := True;
      Log('[CleanUpFile] PFRO delete scheduled: ' + Path);
    end else begin
      Log('[CleanUpFile] Deleted: ' + Path);
    end;
  end;
end;

procedure CurUninstallStepChanged(CurStep: TUninstallStep);
var
  Rc: Integer;
  Retry: Integer;
begin
  if CurStep = usUninstall then begin
    UninstallSystemModified := False;
    UninstallSucceeded := False;
    UninstallNeedsReboot := False;

    BackupPPL;
    RegWriteDWordValue(HKLM, 'SYSTEM\CurrentControlSet\Control\Audio',
                       'DisableProtectedAudioDG', 1);
    UninstallSystemModified := True;

    Exec(ExpandConstant('{cmd}'),
         '/C net stop audiosrv /y',
         '', SW_HIDE, ewWaitUntilTerminated, Rc);

    for Retry := 1 to 15 do begin
      Exec(ExpandConstant('{cmd}'),
           '/C taskkill /F /IM audiodg.exe',
           '', SW_HIDE, ewWaitUntilTerminated, Rc);
      if Rc = 0 then Break;
      Sleep(200);
    end;
    Sleep(500);
  end

  else if CurStep = usPostUninstall then begin
    CleanUpFile(ExpandConstant('{app}\SoundMate_APO.dll'));

    if UninstallNeedsReboot then begin
      MoveFileExDelete(ExpandConstant('{app}'), 0, MOVEFILE_DELAY_UNTIL_REBOOT);
      Log('[usPostUninstall] {app} PFRO delete scheduled.');
    end;

    if not UninstallNeedsReboot then begin
      Exec(ExpandConstant('{cmd}'),
           '/C net start AudioEndpointBuilder & net start audiosrv',
           '', SW_HIDE, ewWaitUntilTerminated, Rc);
      RestorePPL;
    end else begin
      RestorePPL;
    end;

    UninstallSucceeded := True;
  end;
end;

function UninstallNeedRestart(): Boolean;
begin
  Result := UninstallNeedsReboot;
end;

procedure DeinitializeUninstall();
var
  Rc: Integer;
begin
  if (not UninstallSucceeded) and UninstallSystemModified then begin
    Log('[DeinitializeUninstall] Rollback triggered.');
    RestorePPL;
    Exec(ExpandConstant('{cmd}'),
         '/C net start AudioEndpointBuilder & net start audiosrv',
         '', SW_HIDE, ewWaitUntilTerminated, Rc);
  end;
end;
