# cpu_throttle.ps1 — simulate a weaker host CPU for any psxrecomp title.
#
# WHY THIS EXISTS: "runs fine here, players report slowdown there" is not
# reproducible on a fast dev box. Rather than guess, throttle the running
# process until the reported symptom appears, then read the budget off the
# dial. Uses the Windows Job Object CPU rate control (the same hard cap the
# scheduler applies to containers), so it needs no launcher/runtime support
# and works on a LIVE process — never restart a session to arm it.
#
# The job is NAMED, so the cap can be re-dialed at any time from a fresh shell
# without re-assigning the process. A job object outlives the handle that made
# it for as long as a process is still inside it, so this script exits and the
# cap stays on.
#
# Two independent knobs, both reversible while the game runs:
#   -CoresWorth <n>   hard CPU-rate cap, expressed as cores' worth of compute.
#                     This is the "slower clock" dial. 0.5 on a 60fps title
#                     that wants ~1 core is roughly a half-speed CPU.
#   -Cores <n>        processor affinity: how many logical CPUs the process may
#                     run on at all. This is the "fewer cores" dial.
#
# Usage:
#   tools\cpu_throttle.ps1 -ProcessId 51368 -CoresWorth 0.5
#   tools\cpu_throttle.ps1 -ProcessId 51368 -CoresWorth 0.35   # tighten, live
#   tools\cpu_throttle.ps1 -ProcessId 51368 -Cores 2           # 2 cores only
#   tools\cpu_throttle.ps1 -ProcessId 51368 -Measure           # sample, no change
#   tools\cpu_throttle.ps1 -ProcessId 51368 -Off               # remove cap + affinity
#
# NOTE: the hard cap is enforced by withholding scheduling quanta, so a heavily
# throttled process stutters rather than degrading smoothly. That is fine for
# "does the reported hitch appear", and misleading for "what is the real frame
# time on that CPU". Read it as a reproduction tool, not a profiler.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][int]$ProcessId,
    [double]$CoresWorth = 0,
    [int]$Cores = 0,
    [switch]$Measure,
    [switch]$Off,
    [int]$SampleSeconds = 4
)

$ErrorActionPreference = "Stop"

Add-Type @'
using System;
using System.Runtime.InteropServices;

public static class JobCpu {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr CreateJobObjectW(IntPtr sa, string name);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr OpenJobObjectW(uint access, bool inherit, string name);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool SetInformationJobObject(IntPtr job, int infoClass, ref CPU_RATE info, int len);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CloseHandle(IntPtr h);

    [StructLayout(LayoutKind.Sequential)]
    public struct CPU_RATE { public uint ControlFlags; public uint RateOrWeight; }

    // JobObjectCpuRateControlInformation
    public const int InfoClass = 15;
    public const uint ENABLE   = 0x1;
    public const uint HARD_CAP = 0x4;
    // PROCESS_SET_QUOTA | PROCESS_TERMINATE | PROCESS_QUERY_INFORMATION
    public const uint PROC_ACCESS = 0x0100 | 0x0001 | 0x0400;
    public const uint JOB_ALL_ACCESS = 0x1F001F;
}
'@

$proc = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
if (-not $proc) { throw "no process with PID $ProcessId" }
$nCpu = [Environment]::ProcessorCount

function Get-AffinityMask([int]$n) {
    # [math]::Pow(2,$n)-1 overflows Int64 at $n >= 63 and THROWS, so on a host
    # with 64 logical processors -Off and -Cores both died instead of doing
    # anything. Shift instead, and special-case a full 64 because .NET masks a
    # 64-bit shift count to 63 (shifting by 64 quietly yields 1, not 0).
    # Note ProcessorAffinity covers a single processor group, so a >64-CPU host
    # is inherently limited to the group the process is already in.
    if ($n -ge 64) { return [int64]-1 }
    return [int64](([uint64]1 -shl $n) - 1)
}

function Measure-Draw([System.Diagnostics.Process]$p, [int]$secs) {
    $t1 = $p.TotalProcessorTime
    Start-Sleep -Seconds $secs
    $p.Refresh()
    $ms = ($p.TotalProcessorTime - $t1).TotalMilliseconds
    [pscustomobject]@{
        CoresWorth = $ms / ($secs * 1000.0)
        SystemPct  = $ms / ($secs * 1000.0 * $nCpu) * 100
    }
}

Write-Host "process : $($proc.ProcessName) PID $ProcessId  ($($proc.Threads.Count) threads)"
Write-Host "machine : $nCpu logical processors"

if ($Measure) {
    $d = Measure-Draw $proc $SampleSeconds
    Write-Host ("draw    : {0:N2} cores' worth  ({1:N2}% of machine)" -f $d.CoresWorth, $d.SystemPct)
    return
}

$jobName = "psxrecomp_throttle_$ProcessId"

# Reuse the existing job if this PID was already throttled, so re-dialing does
# not nest a second job (nested caps only ever ratchet DOWN — you could never
# loosen one). Otherwise create it and assign the live process once.
# CreateJobObject on an existing NAME returns a handle to that same job and
# sets ERROR_ALREADY_EXISTS -- it does not make a second one. That is a more
# reliable reuse probe than OpenJobObject, which needs the name to still be
# resolvable in this session's namespace and quietly fails when it is not.
# Getting this wrong would nest a second job, and nested CPU caps only ever
# take the MINIMUM, so a loosened dial would silently do nothing.
$ERROR_ALREADY_EXISTS = 183
$job = [JobCpu]::CreateJobObjectW([IntPtr]::Zero, $jobName)
$lastErr = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
if ($job -eq [IntPtr]::Zero) { throw "CreateJobObject failed: $([ComponentModel.Win32Exception]::new($lastErr).Message)" }
$fresh = ($lastErr -ne $ERROR_ALREADY_EXISTS)

if ($fresh) {
    $hProc = [JobCpu]::OpenProcess([JobCpu]::PROC_ACCESS, $false, $ProcessId)
    if ($hProc -eq [IntPtr]::Zero) { throw "OpenProcess failed: $([ComponentModel.Win32Exception]::new([Runtime.InteropServices.Marshal]::GetLastWin32Error()).Message)" }
    if (-not [JobCpu]::AssignProcessToJobObject($job, $hProc)) {
        $e = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        [JobCpu]::CloseHandle($hProc) | Out-Null
        throw "AssignProcessToJobObject failed ($e): $([ComponentModel.Win32Exception]::new($e).Message)"
    }
    [JobCpu]::CloseHandle($hProc) | Out-Null
    Write-Host "job     : created '$jobName' and assigned the live process"
} else {
    Write-Host "job     : reusing '$jobName' (re-dialing, process untouched)"
}

$info = New-Object JobCpu+CPU_RATE
if ($Off) {
    # An all-zero control block is rejected (ERROR_INVALID_PARAMETER): the API
    # validates the union against the flags. Releasing a cap therefore means
    # setting it to the full machine (100% == CpuRate 10000), not clearing it.
    $info.ControlFlags = [JobCpu]::ENABLE -bor [JobCpu]::HARD_CAP
    $info.RateOrWeight = [uint32]10000
    if (-not [JobCpu]::SetInformationJobObject($job, [JobCpu]::InfoClass, [ref]$info, 8)) {
        throw "SetInformationJobObject(off) failed: $([ComponentModel.Win32Exception]::new([Runtime.InteropServices.Marshal]::GetLastWin32Error()).Message)"
    }
    $proc.ProcessorAffinity = [IntPtr](Get-AffinityMask $nCpu)
    Write-Host "cap     : REMOVED (affinity restored to all $nCpu processors)"
} elseif ($CoresWorth -gt 0) {
    # CpuRate is in 1/100 of a percent of TOTAL machine capacity, so a cores'
    # worth figure has to be divided by the processor count. Clamp to the API's
    # valid 1..10000 range; anything below 1 would silently mean "no limit".
    $rate = [int][math]::Round($CoresWorth / $nCpu * 10000)
    if ($rate -lt 1)     { $rate = 1 }
    if ($rate -gt 10000) { $rate = 10000 }
    $info.ControlFlags = [JobCpu]::ENABLE -bor [JobCpu]::HARD_CAP
    $info.RateOrWeight = [uint32]$rate
    if (-not [JobCpu]::SetInformationJobObject($job, [JobCpu]::InfoClass, [ref]$info, 8)) {
        throw "SetInformationJobObject failed: $([ComponentModel.Win32Exception]::new([Runtime.InteropServices.Marshal]::GetLastWin32Error()).Message)"
    }
    Write-Host ("cap     : {0:N2} cores' worth = {1:N2}% of machine (CpuRate {2})" -f $CoresWorth, ($rate/100.0), $rate)
}

if ($Cores -gt 0) {
    if ($Cores -gt $nCpu) { $Cores = $nCpu }
    $mask = Get-AffinityMask $Cores
    $proc.ProcessorAffinity = [IntPtr]$mask
    Write-Host "affinity: $Cores of $nCpu logical processors (mask 0x$('{0:X}' -f $mask))"
}

[JobCpu]::CloseHandle($job) | Out-Null
$d = Measure-Draw $proc $SampleSeconds
Write-Host ("result  : {0:N2} cores' worth  ({1:N2}% of machine)" -f $d.CoresWorth, $d.SystemPct)
