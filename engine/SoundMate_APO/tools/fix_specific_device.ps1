$deviceGuid = "{e635331d-faa4-4f67-b630-0ca3b5b94546}"
$path = "SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$deviceGuid\FxProperties"

Write-Host "Taking ownership of $path"

# Enable TakeOwnership Privilege
$definition = @"
using System;
using System.Runtime.InteropServices;
public class TokenPrivilege {
    [DllImport("advapi32.dll", SetLastError = true)]
    public static extern bool OpenProcessToken(IntPtr ProcessHandle, uint DesiredAccess, out IntPtr TokenHandle);
    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Auto)]
    public static extern bool LookupPrivilegeValue(string lpSystemName, string lpName, out long lpLuid);
    [DllImport("advapi32.dll", SetLastError = true)]
    public static extern bool AdjustTokenPrivileges(IntPtr TokenHandle, bool DisableAllPrivileges, ref TOKEN_PRIVILEGES NewState, uint BufferLength, IntPtr PreviousState, IntPtr ReturnLength);
    [StructLayout(LayoutKind.Sequential)]
    public struct TOKEN_PRIVILEGES {
        public uint PrivilegeCount;
        public long Luid;
        public uint Attributes;
    }
}
"@
Add-Type -TypeDefinition $definition

$TOKEN_ADJUST_PRIVILEGES = 0x0020
$TOKEN_QUERY = 0x0008
$SE_PRIVILEGE_ENABLED = 0x00000002

$hToken = [IntPtr]::Zero
if ([TokenPrivilege]::OpenProcessToken([System.Diagnostics.Process]::GetCurrentProcess().Handle, ($TOKEN_ADJUST_PRIVILEGES -bor $TOKEN_QUERY), [ref]$hToken)) {
    $luid = 0
    if ([TokenPrivilege]::LookupPrivilegeValue($null, "SeTakeOwnershipPrivilege", [ref]$luid)) {
        $tp = New-Object TokenPrivilege+TOKEN_PRIVILEGES
        $tp.PrivilegeCount = 1
        $tp.Luid = $luid
        $tp.Attributes = $SE_PRIVILEGE_ENABLED
        [TokenPrivilege]::AdjustTokenPrivileges($hToken, $false, [ref]$tp, 0, [IntPtr]::Zero, [IntPtr]::Zero)
    }
}

# Take Ownership
$key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($path, [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree, [System.Security.AccessControl.RegistryRights]::TakeOwnership)
$acl = $key.GetAccessControl()
$me = [System.Security.Principal.WindowsIdentity]::GetCurrent().User
$acl.SetOwner($me)
$key.SetAccessControl($acl)

# Grant Full Control
$acl = $key.GetAccessControl()
$rule = New-Object System.Security.AccessControl.RegistryAccessRule($me, "FullControl", "Allow")
$acl.SetAccessRule($rule)
$key.SetAccessControl($acl)

Write-Host "Success! Permissions granted for $deviceGuid"
