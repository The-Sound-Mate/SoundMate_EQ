# ============================================================
#  SoundMate_Address_Master v11.3
#  Forcing CLSID to point to Program Files.
# ============================================================

$definition = @"
using System;
using System.Runtime.InteropServices;

public class PrivilegeManager {
    [DllImport("advapi32.dll", SetLastError = true)]
    public static extern bool OpenProcessToken(IntPtr ProcessHandle, uint DesiredAccess, out IntPtr TokenHandle);
    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Auto)]
    public static extern bool LookupPrivilegeValue(string lpSystemName, string lpName, out long lpLuid);
    [DllImport("advapi32.dll", SetLastError = true)]
    public static extern bool AdjustTokenPrivileges(IntPtr TokenHandle, bool DisableAllPrivileges, ref TOKEN_PRIVILEGES NewState, uint BufferLength, IntPtr PreviousState, IntPtr ReturnLength);
    [StructLayout(LayoutKind.Sequential)]
    public struct TOKEN_PRIVILEGES { public uint PrivilegeCount; public long Luid; public uint Attributes; }
    public static void EnablePrivilege(string privilege) {
        IntPtr hToken;
        OpenProcessToken(System.Diagnostics.Process.GetCurrentProcess().Handle, 0x0020 | 0x0008, out hToken);
        TOKEN_PRIVILEGES tp = new TOKEN_PRIVILEGES();
        tp.PrivilegeCount = 1;
        LookupPrivilegeValue(null, privilege, out tp.Luid);
        tp.Attributes = 0x00000002;
        AdjustTokenPrivileges(hToken, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero);
    }
}
"@

Add-Type -TypeDefinition $definition
[PrivilegeManager]::EnablePrivilege("SeTakeOwnershipPrivilege")

$clsidPath = "SOFTWARE\Classes\CLSID\{EC1CC9CE-FAED-4822-828A-82A81A6F018F}\InprocServer32"
$targetDll = "C:\Program Files\EqualizerAPO\EqualizerAPO.dll"

Write-Host "[*] Taking Ownership of CLSID..." -ForegroundColor Cyan

# Take Ownership of CLSID Key
$key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($clsidPath, [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree, [System.Security.AccessControl.RegistryRights]::TakeOwnership)
if ($key) {
    $acl = $key.GetAccessControl()
    $acl.SetOwner([System.Security.Principal.NTAccount]"Everyone")
    $key.SetAccessControl($acl)
    
    # Grant Full Control
    $acl.SetAccessRuleProtection($false, $false)
    $rule = New-Object System.Security.AccessControl.RegistryAccessRule("Everyone", "FullControl", "Allow")
    $acl.AddAccessRule($rule)
    $key.SetAccessControl($acl)
    
    # Set the Path!
    $key.SetValue("", $targetDll, [Microsoft.Win32.RegistryValueKind]::String)
    $key.Close()
    Write-Host "[+] CLSID Path Unified to $targetDll" -ForegroundColor Green
} else {
    Write-Host "[-] Failed to open CLSID key." -ForegroundColor Red
}

# Also ensure InstallPath is correct
$eapoPath = "SOFTWARE\EqualizerAPO"
$key2 = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($eapoPath, [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree, [System.Security.AccessControl.RegistryRights]::FullControl)
if ($key2) {
    $key2.SetValue("InstallPath", "C:\Program Files\EqualizerAPO", [Microsoft.Win32.RegistryValueKind]::String)
    $key2.Close()
}

# Run the One-Shot again to apply everything
powershell -ExecutionPolicy Bypass -File c:\SoundMate_EQ\FinalOneShot.ps1
