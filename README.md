# ROUVY sous Linux avec Wine, DXVK et BlueZ

Prototype open source permettant d'exécuter la version Windows de ROUVY sous
Linux avec accélération GPU et prise en charge Bluetooth LE/GATT.

> Open-source compatibility layer for running the Windows version of ROUVY on
> Linux with Wine/DXVK and a local BlueZ GATT bridge.

Testé avec ROUVY 4.7.2, Wine 11.19, DXVK 2.6, une NVIDIA RTX 3060, un home
trainer Elite DIRETO XR et des contrôleurs Zwift Click.

## Ce que résout le projet

ROUVY démarrait sous Wine, mais trois incompatibilités empêchaient une séance :

1. un greffon Unity WinRT faisait échouer l'interface au démarrage ;
2. la découverte ZeroConf déclenchait un crash après l'authentification ;
3. le greffon Bluetooth Windows ne pouvait pas exploiter le BLE/GATT de Wine.

Le projet fournit trois DLL de remplacement construites localement :

- `AppUINativePlugin.dll`, un shim Win32 pour l'interface Unity ;
- `DnsZeroConfLib.dll`, qui neutralise la découverte LAN optionnelle ;
- `WclBlePluginCPP.dll`, qui conserve l'ABI attendue par ROUVY et dialogue avec
  un helper Linux utilisant BlueZ par D-Bus.

```text
ROUVY / Unity
  |-- Direct3D 11 -> DXVK -> Vulkan -> GPU
  |-- AppUINativePlugin.dll -> shim Win32
  |-- DnsZeroConfLib.dll -> ZeroConf optionnel désactivé
  `-- WclBlePluginCPP.dll -> UDP/TCP sur 127.0.0.1
                                 |
                                 v
                         rouvy-ble-host.py
                                 |
                                 v
                            D-Bus / BlueZ
                                 |
                                 v
                    home trainer et contrôleurs
```

Le lanceur applique également `MouseWarpOverride=force`, nécessaire au bon
fonctionnement de la souris dans la fenêtre X11 native de ROUVY.

## Prérequis

- Linux avec BlueZ et un adaptateur Bluetooth LE ;
- Wine 64 bits et DXVK ;
- Python 3 avec `dbus` et `gi` ;
- la chaîne MinGW-w64 64 bits ;
- une installation légitime de ROUVY pour Windows.

Sur Ubuntu/Debian, les dépendances de compilation et du helper sont fournies
par les paquets suivants :

```bash
sudo apt install gcc-mingw-w64-x86-64 make python3-dbus python3-gi \
  bluez policykit-1 iproute2
```

Installez Wine et DXVK selon les instructions de votre distribution, puis
installez ROUVY dans un préfixe isolé. Par défaut, le projet utilise :

```text
~/.local/share/wineprefixes/rouvy
```

Vous pouvez choisir un autre emplacement avec `ROUVY_WINEPREFIX`.

## Compilation et installation

```bash
git clone https://github.com/liesai/rouvy-linux-wine.git
cd rouvy-linux-wine
make check
make install
```

`make install` :

- crée une sauvegarde `*.original.dll` de chaque greffon remplacé ;
- installe uniquement les DLL construites depuis ce dépôt ;
- installe le helper dans `~/.local/share/rouvy-linux-wine/` ;
- installe le lanceur `~/.local/bin/rouvy-wine` et son entrée de menu.

Le dépôt ne contient aucun installateur, DLL ou assembly propriétaire de
ROUVY. Une mise à jour de l'application peut remplacer les shims : relancez
alors `make install`.

## Lancement

```bash
rouvy-wine
```

Variables utiles :

```bash
export ROUVY_WINEPREFIX=/chemin/vers/le/prefixe
export ROUVY_WINE_BIN=/chemin/vers/wine
export ROUVY_BLUEZ_ADAPTER=/org/bluez/hci1
```

Le helper et la DLL communiquent uniquement sur `127.0.0.1` : UDP 28765 pour
les annonces et notifications, TCP 28766 pour les opérations GATT.

## Le piège des handles GATT

Le blocage final venait de la différence entre le handle de déclaration d'une
caractéristique et son `ValueHandle`. ROUVY indexe les notifications avec le
premier, tandis que les opérations `READ`, `WRITE`, `SUB` et `UNSUB` utilisent
le second. BlueZ les expose ici comme deux handles consécutifs.

Le pont envoie donc le handle de déclaration pour les notifications et
`handle + 1` pour les opérations. Avec le décalage au mauvais endroit, les
callbacks natifs fonctionnent mais l'interface reste sur « Connecting ».

## Diagnostic

- `/tmp/rouvy-ble-host.log` : BlueZ, connexions et opérations GATT ;
- `/tmp/rouvy-ble-dll.log` : pont Windows ;
- `Player.log` dans le profil Wine : Unity et ROUVY.

Une connexion complète produit notamment `services resolved`, des lignes
`GATT subscribe`, un `WRITE-OK` et un flux de `GATT notify`.

## Désinstallation

```bash
make uninstall
```

Cette commande restaure les trois DLL originales sauvegardées et supprime le
lanceur et le helper installés par le projet.

## Limites

- ABI validée avec ROUVY 4.7.2 uniquement ;
- la découverte ZeroConf/LAN optionnelle est désactivée ;
- le chemin BlueZ par défaut est `/org/bluez/hci0` ;
- matériel testé : Elite DIRETO XR et Zwift Click ;
- le prototype reste expérimental : conservez les sauvegardes originales.

## Licence et marques

Le code de ce dépôt est sous licence MIT. ROUVY est une marque de son
propriétaire. Ce projet communautaire n'est ni affilié, ni approuvé, ni pris
en charge par ROUVY. Aucun composant propriétaire n'est redistribué.
