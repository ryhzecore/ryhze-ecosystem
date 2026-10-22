; Fixed identities: acceptance cannot override any production registration.
!ifdef INSTALLER_ACCEPTANCE
  !if "${INSTALLER_ACCEPTANCE}" != "1"
    !error "INSTALLER_ACCEPTANCE must be exactly 1"
  !endif
  !define PRODUCT_ID "RyhzeInstallerAcceptance"
  !define DISPLAY_NAME "Ryhze Installer Acceptance"
  !define INSTALL_BASENAME "Ryhze-Installer-Acceptance"
  !define PRODUCT_KEY "Software\Ryhze\InstallerAcceptance"
  !define SHORTCUT_NAME "Ryhze Installer Acceptance.lnk"
  !define URI_SCHEME "ryhze-installer-acceptance"
  !define APP_USER_MODEL_ID "Ryhze.InstallerAcceptance"
  !define TOAST_ACTIVATOR_CLSID "{9e4cdc8d-efc1-46fd-8402-b714695407d1}"
!else
  !define PRODUCT_ID "RyhzeEntertainment"
  !define DISPLAY_NAME "Ryhze"
  !define INSTALL_BASENAME "Ryhze-Entertainment"
  !define PRODUCT_KEY "Software\Ryhze\Entertainment"
  !define SHORTCUT_NAME "Ryhze Entertainment.lnk"
  !define URI_SCHEME "ryhze"
  !define APP_USER_MODEL_ID "Ryhze.Entertainment"
  !define TOAST_ACTIVATOR_CLSID "{dc65f38b-6b5d-49e9-ba7c-7d6ce82f2d7d}"
!endif
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_ID}"
!define PROTOCOL_KEY "Software\Classes\${URI_SCHEME}"
!define OWNER_MARKER ".ryhze-install.ini"
!define PROTOCOL_COMMAND '$\"$INSTDIR\app\ryhze.exe$\" $\"%1$\"'
!define UNINSTALL_COMMAND '$\"$INSTDIR\Uninstall.exe$\"'
