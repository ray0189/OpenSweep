; OpenSweep installer hooks: OpenSweepCore.exe is in use while the OpenSweep Core service runs,
; so stop the service before files are replaced and start it again afterwards. The service
; registration (binary path, privileges) is refreshed so updates can add required privileges.
!macro customInit
  nsExec::Exec '"$SYSDIR\sc.exe" stop OpenSweepCore'
  Sleep 3000
!macroend

!macro customInstall
  nsExec::ExecToStack '"$SYSDIR\sc.exe" query OpenSweepCore'
  Pop $0
  Pop $1
  ${If} $0 == 0
    nsExec::Exec '"$SYSDIR\sc.exe" privs OpenSweepCore SeChangeNotifyPrivilege/SeImpersonatePrivilege/SeCreateGlobalPrivilege/SeSystemProfilePrivilege'
    nsExec::Exec '"$SYSDIR\sc.exe" start OpenSweepCore'
  ${EndIf}
!macroend

!macro customUnInit
  nsExec::Exec '"$SYSDIR\sc.exe" stop OpenSweepCore'
  Sleep 3000
!macroend

!macro customUnInstall
  ; Full uninstall (not an update): remove the service registration too.
  ${ifNot} ${isUpdated}
    nsExec::Exec '"$SYSDIR\sc.exe" stop OpenSweepCore'
    Sleep 2000
    nsExec::Exec '"$SYSDIR\sc.exe" delete OpenSweepCore'
  ${endIf}
!macroend
