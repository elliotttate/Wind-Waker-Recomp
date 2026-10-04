# Plan : mappage des manettes et des boutons dans le menu ImGui, détection automatique à chaud

Statut : **implémenté** (2026-10-03, branche `feature/gamepad-mapping`). Voir « Implémentation » à la fin.
Proposition initiale du même jour ci-dessous. Ce plan couvre l'hôte de bureau (Mac, et Windows si le menu y est
compilé). L'iPad a déjà son propre menu de remappage (`apple/ios/src/controller_*`).

> Le projet migre vers [BlueWake](https://github.com/chrissotraidis/bluewake). Une contribution en
> amont devra être proposée là-bas, pas ici.

---

## 1. Ce qui existe déjà (constat)

| Élément | Où | Ce qu'il fait |
| --- | --- | --- |
| Menu d'options ImGui | `runtime/host/src/settings_menu.cpp` | Onglets Display / Gameplay / Controls, ouvert par F1, Esc ou Back. Les réglages sont des variables d'environnement sauvegardées dans `settings.ini`. L'onglet Controls ne fait qu'**afficher** les contrôles en dur (puces de texte). |
| Événements SDL | `mouse_camera.c:221` (`dol_aurora_set_event_observer`) → `bluewake_settings_menu_event` | Le menu reçoit tous les événements SDL ; seul `SDL_GAMEPAD_BUTTON_BACK` est traité. |
| Branchement à chaud | Aurora `lib/window.cpp:212` | `SDL_EVENT_GAMEPAD_ADDED/REMOVED` → `input::add_controller` / `remove_controller`, puis `apply_port_preferences()`. |
| Attribution des ports | Aurora `lib/input.cpp` | Préférences de port persistées (GUID + numéro de série). **Le repli « première manette libre → port 0 » est désactivé (`#if 0` dans `get_controller_for_player`)** : une manette à laquelle SDL ne donne pas d'indice de joueur n'est sur aucun port, donc le jeu ne la voit pas. |
| API de mappage | Aurora `include/dolphin/pad.h` | `PADSetButtonMapping`, `PADSetAxisMapping`, `PADGetButtonMappings`, `PADGetNativeButtonPressed`, `PADGetNativeAxisPulled`, `PADGetDeadZones`, `PADRestoreDefaultMapping`, `PADSerializeMappings`, `PADSetPortForIndex`, `PADGetControllerType`, `PADGetBatteryState`, `PADBlockInput`, `PADSetKeyButtonBinding`… |
| Persistance du mappage | Aurora `PADSerializeMappings` | Un fichier `<nom>_<VID>_<PID>.controller` par modèle de manette dans `userPath`, rechargé automatiquement à la connexion (`EnsureMappingLoaded`). |
| Remappage iOS | `apple/ios/src/controller_apply.cpp` | Utilise l'API ci-dessus : 6 boutons (A B X Y Z Start), inversion de la caméra. Interroge le port 0 deux fois par seconde pour réappliquer le mappage. |
| Actions de l'hôte | `jump_button.c:286`, `sprint.c:103`, `mouse_camera.c:419`, `haptics.c:227` | Lisent **directement toutes les manettes SDL** avec des boutons codés en dur (LB = saut, L3 = sprint, R3 = 1re personne, D-pad = zoom). Elles ignorent les ports et tout remappage. |

Conclusion : il n'y a presque rien à écrire dans Aurora. Le travail se fait surtout dans l'hôte :
1. un **registre de manettes** qui suit les branchements et garantit que le port 0 a toujours une manette ;
2. une **table de liaisons** unique (boutons GameCube et actions de l'hôte) ;
3. un **onglet « Manettes »** dans le menu ImGui, avec capture « appuyez sur un bouton » ;
4. une **adaptation au type de manette** (libellés, mappage par défaut, vibrations).

---

## 2. Architecture cible

```
SDL_EVENT_GAMEPAD_ADDED / REMOVED / BUTTON_DOWN / AXIS_MOTION
        │  (Aurora : add_controller / remove_controller, avant l'observateur)
        ▼
mouse_camera.c observe() ──► gamepads.c  bluewake_gamepads_event()
                               │  registre : appareils, type, « dernier utilisé »
                               │  politique d'attribution du port 0 (PADSetPortForIndex)
                               │  application du profil (PADSet*Mapping) à la connexion
                               ▼
                         input_bindings.c   table de liaisons
                               │  GameCube : délègue à Aurora (PADButtonMapping)
                               │  Hôte : JUMP, SPRINT, FIRST_PERSON, MENU, ZOOM_IN/OUT
                               ▼
          jump_button.c / sprint.c / mouse_camera.c / settings_menu.cpp
          (lisent bluewake_action_down(ACTION) au lieu de SDL_GetGamepadButton)
                               ▲
settings_menu.cpp  onglet « Controllers » ─ liste, port, remappage, zones mortes, profils
```

### Nouveaux fichiers

- `runtime/host/src/gamepads.{c,h}` : registre et branchement à chaud.
- `runtime/host/src/input_bindings.{c,h}` : actions de l'hôte, valeurs par défaut par type de manette, chargement et sauvegarde.
- `runtime/host/src/controller_glyphs.{c,h}` : libellés par famille (Xbox, PlayStation, Nintendo, GameCube).
- `tests/gamepad_hotplug_test.c` : tests avec des manettes virtuelles SDL.

À ajouter dans `runtime/host/CMakeLists.txt`, à côté de `settings_menu.cpp`.

---

## 3. Étapes

### Étape 1 : registre de manettes et détection à chaud (`gamepads.c`)

**API**

```c
typedef struct BWGamepadInfo {
    SDL_JoystickID id;
    u32 aurora_index;            // index pour PADSetPortForIndex / PADGetNameForControllerIndex
    char name[128];
    PADControllerType type;      // PADGetControllerTypeForIndex
    int port;                    // -1 si aucun
    bool is_gc_adapter;
    PADBatteryState battery; float battery_pct;
    Uint64 last_input_ms;        // dernière entrée réelle (hors bruit des axes)
} BWGamepadInfo;

void bluewake_gamepads_install(void);
void bluewake_gamepads_event(const SDL_Event* e);   // appelé depuis observe()
void bluewake_gamepads_tick(void);                  // une fois par image (overlay draw)
int  bluewake_gamepads_count(void);
const BWGamepadInfo* bluewake_gamepads_get(int i);
SDL_Gamepad* bluewake_gamepads_for_port(u32 port);  // remplace les boucles SDL_GetGamepads des modules
void bluewake_gamepads_assign(u32 port, SDL_JoystickID id);  // choix explicite du menu
```

**Politique d'attribution du port 0** (réglage `BLUEWAKE_PAD_PORT0=auto|last|fixed`) :

- `auto` (par défaut) : si le port 0 est libre et qu'une manette se connecte, elle le prend
  (`PADSetPortForIndex(index, 0)`). Si la manette du port 0 se débranche, la suivante connectée
  la remplace. Cela compense le `#if 0` d'Aurora sans le modifier.
- `last` : la dernière manette à avoir appuyé sur un bouton (ou bougé un stick au-delà de 50 %)
  prend le port 0. C'est pratique quand on change de manette en cours de partie.
- `fixed` : on garde la préférence enregistrée par Aurora (GUID et numéro de série) ; les autres
  manettes sont ignorées.

**Comportement lors des événements**

- `GAMEPAD_ADDED` : enregistrer l'appareil, appliquer la politique, appliquer le profil (étape 3)
  et afficher une notification ImGui de 3 s : « Manette DualSense connectée — joueur 1 ».
- `GAMEPAD_REMOVED` : si c'était la manette du port 0, **mettre le jeu en pause en ouvrant le
  menu** (réglage `BLUEWAKE_PAD_PAUSE_ON_DISCONNECT`, activé par défaut), comme sur console.
  Puis réattribuer le port selon la politique.
- `GAMEPAD_REMAPPED` (base SDL mise à jour) : réappliquer le profil.
- Tenir compte de l'adaptateur GameCube : 4 appareils avec le même VID et PID ; on ne remappe pas
  par défaut (`PADIsGCAdapter`).

**À vérifier d'abord** : que l'observateur d'événements (`dol_aurora_set_event_observer`) est bien
appelé *après* `input::add_controller` d'Aurora. Sinon, différer le traitement à
`bluewake_gamepads_tick()`. Le plus simple et le plus sûr est de comparer à chaque image la liste
`PADCount()` à celle de l'image précédente, comme le fait déjà l'iPad avec son polling, et de ne
garder les événements que pour réagir sans attendre.

### Étape 2 : table de liaisons (`input_bindings.c`)

Deux familles :

1. **Boutons et axes GameCube** (A, B, X, Y, Z, L, R, Start, D-pad, stick, C-stick, gâchettes) :
   on ne les duplique pas. On les lit et on les écrit dans Aurora (`PADGetButtonMappings`,
   `PADSetButtonMapping`, `PADSetAxisMapping`), qui les applique dans `PADRead` et les persiste par
   modèle.
2. **Actions de l'hôte**, qui n'existent pas sur GameCube :

   | Action | Défaut (manette) | Défaut (clavier) | Module |
   | --- | --- | --- | --- |
   | `JUMP` | LB | Espace | `jump_button.c` |
   | `SPRINT` | L3 | Shift | `sprint.c` |
   | `FIRST_PERSON` | R3 | — | `mouse_camera.c` |
   | `ZOOM_IN` / `ZOOM_OUT` | D-pad haut/bas (télescope) | molette | `mouse_camera.c` |
   | `MENU` | Back/Select | F1 / Esc | `settings_menu.cpp` |
   | `SAVE_STATE` / `LOAD_STATE` | aucun | F5 / F9 | `main.c` |

   ```c
   typedef enum { BW_ACT_JUMP, BW_ACT_SPRINT, BW_ACT_FIRST_PERSON, BW_ACT_ZOOM_IN,
                  BW_ACT_ZOOM_OUT, BW_ACT_MENU, BW_ACT_SAVE_STATE, BW_ACT_LOAD_STATE,
                  BW_ACT_COUNT } BWAction;
   bool bluewake_action_down(BWAction a);   // manette du port 0 + clavier
   int  bluewake_action_binding(BWAction a);        // SDL_GamepadButton ou -1
   void bluewake_action_bind(BWAction a, int sdl_button);
   ```

   Ensuite, **remplacer** dans `jump_button.c:286`, `sprint.c:103-108` et `mouse_camera.c:419-423` les
   boucles `SDL_GetGamepads` et les `SDL_GetGamepadButton(pad, CONSTANTE)` par
   `bluewake_action_down(...)`. Cela corrige aussi un défaut actuel : avec deux manettes branchées,
   celle du joueur 2 déclenche le saut et le sprint de Link.

**Conflits** : si on assigne un bouton natif déjà utilisé par une autre action GameCube, on fait un
échange (même règle que `bluewake_remap_set` sur iOS). Un même bouton peut servir à une action GC
et à une action de l'hôte : c'est déjà le cas aujourd'hui avec LB, qui sert au saut et n'est lié à
aucun bouton GC. Le menu affiche alors un avertissement, sans bloquer.

**Sécurité** : `MENU` ne peut pas rester sans bouton. On garde toujours un recours codé en dur :
**Start + Back maintenus 1 s** ouvre le menu, quel que soit le mappage.

### Étape 3 : adaptation au type de manette

- **Mappage par défaut selon le type** : au démarrage, avant `PADInit`, appeler
  `PADSetDefaultMapping()` pour `PAD_TYPE_SWITCH_PROCON` / `JOYCON_PAIR` / `NSO_GAMECUBE`, afin de
  choisir entre « par position » (bouton du bas = A GameCube) et « par libellé » (le A Nintendo
  = A). Réglage `BLUEWAKE_PAD_NINTENDO_LAYOUT=position|label`.
- **Libellés (`controller_glyphs.c`)** : `SDL_GetGamepadButtonLabel()` + `PADGetControllerType()`
  donnent les noms affichés : « A / Croix / B » pour le bouton du bas selon la famille, « LB / L1 / L »,
  etc. Ils servent à l'onglet Manettes et aux puces de l'onglet Controls, qui deviennent
  dynamiques : on n'écrit plus « Left bumper jump » en dur, on écrit le nom du bouton réellement
  lié pour la manette branchée.
- **Vibrations** : `haptics.c` sait déjà gérer la DualSense. On lui fait utiliser
  `bluewake_gamepads_for_port(0)` au lieu de chercher dans toutes les manettes, et on désactive les
  options de vibration dans le menu si `PADSupportsRumbleIntensity(0)` est faux.
- **LED** : `PADHasLED` / `PADSetColor` pour colorer la DualSense selon le joueur (optionnel).
- **Gyroscope** : hors de cette étape. La capacité (`PADHasSensor`) est simplement notée dans le
  registre pour une visée gyroscopique plus tard.

### Étape 4 : onglet « Controllers » dans le menu ImGui

On ajoute un `BeginTabItem("Controllers")` dans `settings_menu.cpp:draw()`, entre « Gameplay » et
« Controls ». L'ancien onglet « Controls » garde la caméra, la souris et les vibrations.

```
┌ Controllers ─────────────────────────────────────────────────────────┐
│ Connected                                                            │
│  ● DualSense Wireless Controller   PS5    Player 1   🔋 78 %         │
│  ○ Xbox Wireless Controller        XboxOne  —        [Use as P1]     │
│ Player 1 controller: [ Auto (newest connected) ▾ ]                    │
│ ☑ Pause when the controller disconnects                              │
├──────────────────────────────────────────────────────────────────────┤
│ Buttons — DualSense (saved for this controller model)                │
│  GameCube A        [ Cross       ]   ← click, then press a button    │
│  GameCube B        [ Square      ]                                    │
│  …  X Y Z L R Start D-pad                                             │
│ Wind Waker Recomp actions                                             │
│  Jump              [ L1          ]                                    │
│  Sprint            [ L3          ]   …                                │
│ Sticks                                                                │
│  Control stick     [ Left stick  ]   Dead zone ──●──── 15 %          │
│  C-stick           [ Right stick ]   Dead zone ──●──── 15 %          │
│  Triggers          analog ☑   threshold ──●── 30 %                    │
│ [Reset this controller]  [Test input]                                 │
└──────────────────────────────────────────────────────────────────────┘
```

**Capture d'un bouton** (le point délicat) :

1. On clique sur un bouton de liaison. On passe en état `capturing = {target, start_ms}` et on
   affiche « Press a button on <manette>… (Esc to cancel, 5 s) ».
2. Pendant la capture :
   - `io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad`, sinon la pression valide aussi le
     widget ImGui ou déplace le focus ;
   - `bluewake_settings_menu_event` consomme les `GAMEPAD_BUTTON_DOWN` (y compris Back, qui ne doit
     pas fermer le menu) ;
   - **on attend d'abord que tous les boutons soient relâchés**, puis on lit
     `PADGetNativeButtonPressed(port)` ou l'événement `gbutton.button` ;
   - pour un axe : `PADGetNativeAxisPulled(port)` avec un seuil de 60 % ; pour une gâchette, on
     accepte l'axe ou le bouton numérique.
3. On applique avec `PADSetButtonMapping` / `bluewake_action_bind` (échange en cas de conflit), puis
   on restaure la navigation à la manette.
4. Le délai de 5 s ou Esc annulent. Une manette débranchée pendant la capture annule aussi.

**Test des entrées** : un panneau qui affiche le `PADStatus` du port 0 tel que le jeu le lit (on
appelle `PADRead` ou on lit le dernier `live_pad` de `main.c`) avec des pastilles A, B, X… et deux
cercles pour les sticks. C'est très utile pour régler les zones mortes.

**Clavier** : la même table avec `PADSetKeyButtonBinding` / `PADGetKeyButtonBindings` (Aurora
gère déjà les liaisons clavier). La capture se fait par `SDL_EVENT_KEY_DOWN`. C'est une sous-section
repliable, à faire en deuxième.

### Étape 5 : persistance

- **GameCube (par modèle de manette)** : `PADSerializeMappings()` à la fermeture du menu (dans
  `close_menu()`, à côté de `save()`). Aurora recharge le profil à la connexion du même modèle.
  Il faut vérifier que `aurora::g_config.userPath` pointe bien vers
  `~/Library/Application Support/Wind Waker Recomp/` sur Mac (et `%APPDATA%\BlueWake` sur Windows).
- **Actions de l'hôte et politique** : dans `settings.ini`, ajouter à `kKeys` :
  `BLUEWAKE_PAD_PORT0`, `BLUEWAKE_PAD_PAUSE_ON_DISCONNECT`, `BLUEWAKE_PAD_NINTENDO_LAYOUT`,
  `BLUEWAKE_PAD_ACTIONS` (format `jump=leftshoulder,sprint=leftstick,…`, avec les noms de
  `SDL_GetGamepadStringForButton`, lisibles et stables).
  Profil par modèle (facultatif, v2) : `BLUEWAKE_PAD_ACTIONS.<VID>_<PID>=…`.
- **Préférence de port** : déjà persistée par Aurora (`persist_controller_for_player` via
  `PADSetPortForIndex`). En mode `auto`, ne **pas** l'écrire à chaque branchement, sinon le mode
  devient `fixed` sans qu'on le veuille. Il faut donc passer par `input::set_player_index` (sans
  persistance), soit avec un petit patch Aurora qui expose `PADSetPortForIndexTransient`, soit en
  appelant `SDL_SetGamepadPlayerIndex` directement sur `PADGetSDLGamepadForIndex(i)`.

### Étape 6 : le jeu s'adapte en cours de partie

- **Sans redémarrage** : toutes les modifications s'appliquent tout de suite. Aurora lit son mappage
  à chaque `PADRead` et les modules de l'hôte lisent `bluewake_action_down` à chaque image.
  Aucun « (next launch) » dans cet onglet.
- **Manette ↔ clavier** : le patch `0083` combine déjà le clavier et la manette sur les axes. On ajoute
  seulement un indicateur « dernier périphérique utilisé » dans le registre, qui sert à l'affichage
  des libellés.
- **Changement de manette en jeu** : en mode `last`, l'attribution change dès la première pression.
  On met `PADClearVirtualStatus(0)` et une image avec les entrées remises à zéro, pour éviter qu'un
  bouton reste « collé » au moment du changement.
- **Aucune manette** : le clavier et la souris continuent de fonctionner. L'onglet affiche
  « No controller — connect one at any time ».

### Étape 7 : tests

- `tests/gamepad_hotplug_test.c` : `SDL_AttachVirtualJoystick` (déjà utilisé dans
  `haptics.c:347`) pour créer et détacher des manettes de type Xbox ou PS5 sans matériel. On vérifie :
  - la connexion → port 0 en mode `auto` ;
  - la déconnexion → la seconde manette prend le port 0 et le menu s'ouvre si l'option est activée ;
  - le mode `last` → le port 0 suit la dernière manette qui appuie ;
  - remappage A↔B → `PADRead` renvoie `PAD_BUTTON_B` quand on appuie sur le bouton du bas ;
  - une action de l'hôte remappée (saut sur RB) → `bluewake_action_down(JUMP)`.
- Étendre `tests/headless_pad_test.c` pour la lecture et l'écriture de `BLUEWAKE_PAD_ACTIONS`.
- Le menu : `BLUEWAKE_SETTINGS_TEST_OPEN` existe déjà. On ajoute une variable de test
  `BLUEWAKE_SETTINGS_TEST_TAB=controllers` pour une capture d'écran automatique.
- Tests manuels : DualSense (USB et Bluetooth), Xbox Series, Switch Pro, adaptateur GameCube Mayflash
  ou officiel, 8BitDo. Branchement et débranchement pendant une cinématique, pendant la capture et
  menu ouvert.

---

## 4. Ordre de livraison (petites PR indépendantes)

1. **Registre et attribution automatique du port 0** (étape 1), avec les tests. C'est le plus
   utile tout de suite : aujourd'hui une manette sans indice de joueur SDL n'atteint pas le jeu.
2. **Actions de l'hôte rattachées au port 0** (étape 2, sans interface) : corrige le cas « la manette
   du joueur 2 fait sauter Link ».
3. **Onglet Controllers en lecture seule** : liste, type, batterie, choix du joueur 1, pause à la
   déconnexion.
4. **Remappage GameCube et capture** (étape 4) + `PADSerializeMappings`.
5. **Remappage des actions de l'hôte** + libellés selon le type de manette (étape 3).
6. **Zones mortes, test des entrées, clavier.**
7. Facultatif : mise en commun avec iOS (`controller_apply.cpp` pourrait utiliser
   `input_bindings.c`) et gestion des ports 2 à 4.

## 5. Risques et questions ouvertes

- **Ordre des événements** entre Aurora et l'observateur de l'hôte : à vérifier en premier (étape 1).
- **Windows** : le menu est décrit comme « Mac host » dans `settings_menu.h`, et `default_path()`
  utilise `$HOME/Library/…`. Il faut savoir si le menu est compilé et utilisé sur Windows (le
  README parle de F1 sur les deux plateformes) avant d'y ajouter la persistance.
- **Persistance du port en mode auto** : voir l'étape 5. Un patch Aurora minime est peut-être
  nécessaire (`patches/recompcore/01xx-…`, à déclarer dans `config/recompcore-patches.json`).
- **Steam Input** : sous Steam, les manettes apparaissent comme des manettes Xbox virtuelles ;
  les libellés seront ceux de Xbox. Il faut le documenter, pas le corriger.
- **Navigation ImGui** : le bouton Back ouvre et ferme le menu ; s'il devient remappable, il faut
  conserver le recours Start + Back.

---

## 6. Implémentation (2026-10-03)

| Étape | Fichiers | Écarts par rapport au plan |
| --- | --- | --- |
| 1. Registre et branchement à chaud | `runtime/host/src/gamepads.{c,h}` | Le registre n'utilise que l'API SDL : le port est l'indice de joueur SDL, comme dans Aurora. Il est donc testable sans Aurora. Il est alimenté par l'observateur d'événements (`mouse_camera.c`, appelé après qu'Aurora a ouvert ou fermé la manette, ce qui a été vérifié dans `window.cpp` et `aurora_backend.cpp`) et resynchronisé à chaque image (`settings_menu.cpp:draw`). |
| 2. Actions de l'hôte | `runtime/host/src/input_bindings.{c,h}` ; `jump_button.c`, `sprint.c`, `mouse_camera.c`, `haptics.c` | Ces modules lisent la manette du joueur 1 (ou n'importe laquelle tant que le joueur 1 n'en a pas). La caméra rapide et le sprint lisent les sticks *à travers le mappage* : inverser les sticks déplace aussi la caméra. Les vibrations vont à la manette du joueur 1. |
| 3. Adaptation au type de manette | `runtime/host/src/controller_glyphs.{c,h}` ; `pad_remap.cpp` | Pas de `PADSetDefaultMapping` ni de `BLUEWAKE_PAD_NINTENDO_LAYOUT` : un bouton préréglé « A, B, X and Y as printed » s'affiche pour les manettes Nintendo, et le résultat est sauvegardé avec le reste du mappage. |
| 4. Onglet Controllers | `runtime/host/src/settings_menu.cpp` | Comme prévu, plus des notifications de connexion et de déconnexion en haut à droite, et un résumé dynamique dans l'onglet Controls. |
| 5. Persistance | `pad_remap.cpp` (Aurora `PADSerializeMappings` à la fermeture du menu) ; `settings.ini` (`BLUEWAKE_PAD_PORT0`, `BLUEWAKE_PAD_PAUSE_ON_DISCONNECT`, `BLUEWAKE_PAD_ACTIONS`, `BLUEWAKE_KEY_ACTIONS`) | Aucun patch Aurora : en modes `auto` et `last`, l'attribution passe par `SDL_SetGamepadPlayerIndex` (temporaire) ; en mode `fixed`, par `PADSetPortForIndex` (persistant). Quitter le mode `fixed` appelle `PADClearPort`. |
| 6. Adaptation en cours de partie | tout ce qui précède | Au changement de joueur 1, `PADBlockInput(true/false)` : Aurora ignore les boutons encore tenus jusqu'à ce qu'ils soient relâchés. |
| 7. Tests | `tests/gamepads_test.c` (cible CTest `bluewake_gamepads_test`) | Manettes virtuelles SDL : connexion et déconnexion, trois politiques, pause, actions, boutons d'une manette GameCube, analyse et écriture des réglages, libellés. |

**Vérifié** :
- `bluewake_gamepads_test` passe (SDL 3.4.10, celui qu'Aurora épingle).
- `settings_menu.cpp`, `pad_remap.cpp` et les modules modifiés compilent sans avertissement (`-Wall -Wextra`) avec les en-têtes réels de RecompCore `8ab24dae` (patch 0170 appliqué), d'ImGui 1.91.9b et de SDL 3.4.10.
- Un harnais hors écran (non versionné) a fait le rendu du vrai menu avec ImGui et une fausse couche PAD. Il a exercé la capture de bouton avec échange, l'attente du relâchement, la capture d'axe, de touche et d'action, Échap, la pause à la déconnexion, Start + Back et la sauvegarde à la fermeture, sans aucun assert ImGui.

**Non vérifié** :
- La compilation complète de l'app, qui demande le disque et le code recompilé.
- La compilation iOS.
- Windows : `settings_menu.h` décrit toujours le menu comme celui du Mac.
- Les manettes réelles (DualSense, Xbox, Switch Pro, adaptateur GameCube).
