# Fil X — ROUVY sous Linux avec Wine et BlueZ

## 1/8

ROUVY n’a pas de client Linux, mais sa version Windows fonctionne maintenant
chez moi sous Wine : accélération GPU, home trainer et contrôleurs Bluetooth
compris. Voici les trois obstacles qu’il a fallu lever. 🚴🐧

## 2/8

Le rendu était la partie facile : Direct3D 11 passe par DXVK vers Vulkan sur
une RTX 3060. Le tout tourne dans un préfixe Wine isolé, sans VM et sans Android.

## 3/8

Premier blocage : un plugin Unity initialise `Windows.UI.ViewManagement` via
WinRT. Sous Wine, l’activation échoue. Un petit shim garde la même ABI et
remplace les fonctions utiles par leurs équivalents Win32.

## 4/8

Deuxième blocage : crash juste après le login. Le plugin ZeroConf appelle
`DnsServiceBrowse()`, incomplet dans Wine. Cette découverte LAN étant
optionnelle, un second shim la désactive proprement sans toucher au BLE.

## 5/8

Le gros morceau : le Bluetooth. Une DLL remplace le plugin Windows de ROUVY et
parle en localhost avec un helper Python. Celui-ci traduit scan, connexion,
lectures, écritures et notifications GATT vers BlueZ/D-Bus.

## 6/8

Le bug final tenait à `+1` : ROUVY indexe les notifications avec le handle de
déclaration GATT, mais les opérations utilisent le ValueHandle. Au mauvais
endroit, tout semble connecté côté natif et l’UI reste sur « Connecting ».

## 7/8

Après correction : services résolus, handshake terminé, état Connected, puis
puissance, cadence et vitesse. Validé avec un Elite DIRETO XR et des Zwift
Click. Le lanceur corrige aussi la souris DirectInput sous X11.

## 8/8

Le prototype est publié sous licence MIT, sans aucun binaire propriétaire de
ROUVY. Il cible pour l’instant ROUVY 4.7.2, Wine 11.19 et BlueZ :

https://github.com/liesai/rouvy-linux-wine

#Linux #Wine #ROUVY #BlueZ
