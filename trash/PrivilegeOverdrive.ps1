# ============================================================
#  SoundMate_PowerShell_Privilege_Overdrive v10.3
#  Forcing Registry Ownership via .NET Privilege Activation.
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
    public struct TOKEN_PRIVILEGES {
        public uint PrivilegeCount;
        public long Luid;
        public uint Attributes;
    }

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

$EAPO_CLSID = "{EC1CC9CE-FAED-4822-828A-82A81A6F018F}"
$RENDER_KEY = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render"

Write-Host "[*] Privilege Escalated. Grafting Official Structure..." -ForegroundColor Cyan

Get-ChildItem $RENDER_KEY | ForEach-Object {
    $guid = $_.PSChildName
    $fxPath = "$RENDER_KEY\$guid\FxProperties"
    
    if (Test-Path $fxPath) {
        Write-Host "[*] Taking Ownership: $guid"
        
        # Take Ownership via .NET Registry Security
        $key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($fxPath.Replace("HKLM:\", ""), [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree, [System.Security.AccessControl.RegistryRights]::TakeOwnership)
        $acl = $key.GetAccessControl()
        $owner = [System.Security.Principal.NTAccount]"Everyone"
        $acl.SetOwner($owner)
        $key.SetAccessControl($acl)
        
        # Grant Full Control
        $acl.SetAccessRuleProtection($false, $false)
        $rule = New-Object System.Security.AccessControl.RegistryAccessRule("Everyone", "FullControl", "ContainerInherit,ObjectInherit", "None", "Allow")
        $acl.AddAccessRule($rule)
        $key.SetAccessControl($acl)
        $key.Close()

        # Graft official E-APO Structure
        Set-ItemProperty -Path $fxPath -Name "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1" -Value $EAPO_CLSID -Type String
        Set-ItemProperty -Path $fxPath -Name "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2" -Value $EAPO_CLSID -Type String
        
        $childPath = "$fxPath\Child APOs"
        if (!(Test-Path $childPath)) { New-Item -Path $childPath -Force }
        Set-ItemProperty -Path $childPath -Name "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1" -Value $EAPO_CLSID -Type String
        Set-ItemProperty -Path $childPath -Name "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2" -Value $EAPO_CLSID -Type String
        
        Set-ItemProperty -Path $fxPath -Name "{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5" -Value 1 -Type DWord
    }
}

Write-Host "[+] v10.3 Privilege Overdrive Successful!" -ForegroundColor Green
