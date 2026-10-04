Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class CadWindows {
  public delegate bool Callback(IntPtr hwnd, IntPtr parameter);
  [DllImport("user32.dll")] public static extern bool EnumWindows(Callback callback, IntPtr parameter);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr hwnd, StringBuilder text, int length);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hwnd, int command);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
}
'@
$script:found = @()
[CadWindows]::EnumWindows({
  param($hwnd, $parameter)
  $text = New-Object System.Text.StringBuilder 512
  [void][CadWindows]::GetWindowText($hwnd, $text, 512)
  if ($text.ToString() -like '*win31c*') {
    [void][CadWindows]::ShowWindow($hwnd, 9)
    $foreground = [CadWindows]::SetForegroundWindow($hwnd)
    $script:found += [PSCustomObject]@{Title=$text.ToString();Visible=[CadWindows]::IsWindowVisible($hwnd);Foreground=$foreground}
  }
  return $true
}, [IntPtr]::Zero) | Out-Null
$script:found
