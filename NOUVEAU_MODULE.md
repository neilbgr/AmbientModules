# Créer un nouveau module AmbientModules

Checklist et conventions établies au fil du projet, pour ne rien oublier lors de l'ajout d'un nouveau module (inspiré d'une section du SOLAR 42F ou non).

## 1. Classe(s) DSP réutilisable(s)

Dans `src/dsp/<Nom>Core.hpp` (ou `<Nom>.hpp`) : header-only, `#pragma once`, **aucune dépendance à `Module`/param/light** — juste du calcul pur (`process(sampleTime, ...)` qui retourne une valeur). Exemples existants : `DroneVoice`, `AREnvelope`, `ADSREnvelope`, `TriSquareLFO`, `SolarVCOCore`. Ça permet de réutiliser le moteur dans un autre module plus tard sans dépendre du premier.

## 2. Le module lui-même : `src/<Nom>.cpp`

- `enum ParamIds / InputIds / OutputIds / LightIds` avec `NUM_PARAMS` etc. en dernier.
- Constructeur : `config(...)` puis un `configParam`/`configSwitch`/`configInput`/`configOutput` par élément, avec un label clair (et une unité si pertinent, voir section CPU/affichage plus bas).
- `process()` : lire les params/inputs, appeler la ou les classes DSP, écrire les outputs/lights.
- Un `struct <Nom>Widget : ModuleWidget` avec les positions des composants en `mm2px(Vec(x, y))` — **coordonnées placeholder au départ**, c'est Neil qui finalise le visuel dans Inkscape ensuite.

## 3. Enregistrement du modèle (3 fichiers, ne pas en oublier un)

- `src/plugin.hpp` : `extern Model *model<Nom>;`
- `src/plugin.cpp` : `p->addModel(model<Nom>);` dans `init()`
- `plugin.json` : ajouter un objet dans `"modules"` (`slug`, `name`, `description`, `tags`)

Aucun changement de `Makefile` nécessaire : `SOURCES += $(wildcard src/*.cpp)` prend automatiquement tout nouveau `.cpp`.

## 4. ⚠️ Intégration dans Cardinal (`plugins/plugins.cpp`)

**Étape à ne pas oublier**, sinon `CardinalNative.exe` (ou tout autre variant Cardinal) plante au démarrage avec :
```
terminate called after throwing an instance of 'rack::Exception'
  what():  Manifest contains module <Nom> but it is not defined in plugin
```
Contrairement aux ~90 autres plugins tiers de `plugins/` (dont l'agrégation dans `plugins/plugins.cpp` a été générée une fois par les mainteneurs de Cardinal), **AmbientModules est maintenu à la main** dans ce fichier. Il faut donc ajouter chaque nouveau modèle dans `initStatic__AmbientModules()` :

```cpp
static void initStatic__AmbientModules()
{
    Plugin* const p = new Plugin;
    pluginInstance__AmbientModules = p;

    const StaticPluginLoader spl(p, "AmbientModules");
    if (spl.ok())
    {
        p->addModel(modelBlank);
        p->addModel(modelSolar50Drone);
        p->addModel(modelSolarLFO);
        p->addModel(modelSolarVCO);
        p->addModel(model<Nom>);   // <- à ajouter ici
    }
}
```
(`plugins/Makefile` n'a rien à changer : `PLUGIN_FILES += $(wildcard AmbientModules/src/*.cpp)` récupère déjà le nouveau `.cpp` automatiquement.)

## 5. Panel SVG (`res/<Nom>.svg`)

- Créer un placeholder minimal (rectangle, comme `Blank.svg`), sans `<text>` : **nanosvg (le moteur de rendu SVG de Rack) ne supporte pas les balises `<text>`** — tout texte doit être converti en chemin (path) dans Inkscape avant d'être visible dans Rack.
- Le visuel final est géré par Neil dans Inkscape — pas la peine de peaufiner les coordonnées du placeholder.
- Après une modif du SVG en cours de session Rack : **Rack met les SVG en cache pour la durée du process** (`src/Rack/src/window/Svg.cpp`), donc il faut fermer et rouvrir Rack pour voir un changement, pas juste recharger le patch.

## 6. Bonnes pratiques CPU (priorité affichée du projet : consommation la plus basse possible)

- Jamais de `std::pow(base, exposantVariable)` par sample avec un exposant qui change à l'exécution (knob, CV...) — ça compile vers un vrai appel `powf` coûteux. Utiliser `dsp::approxExp2_taylor5(x)` (approximation polynomiale bon marché de 2^x) à la place.
- Piège : `approxExp2_taylor5` veut un argument non-négatif. Idiome utilisé partout dans le projet :
  ```cpp
  float valeur = dsp::approxExp2_taylor5(exposant + 30.f) / std::pow(2.f, 30.f);
  ```
  (le `std::pow(2.f, 30.f)` a deux arguments littéraux donc il est calculé à la compilation, coût nul à l'exécution).
- Pas besoin de `dsp::ClockDivider` pour throttler ce calcul : `approxExp2_taylor5` est déjà assez léger pour tourner à chaque sample.
- Si un `sin()` ou un calcul de forme d'onde coûteux dépend uniquement d'un output : le sauter si `outputs[X].isConnected()` est faux (gain réel, déjà vérifié en pratique sur SolarLFO/SolarVCO). Attention : ne pas sauter l'incrémentation de la phase elle-même si on veut éviter un saut audible à la reconnexion.
- Pour vérifier après-coup qu'un `pow()` runtime a bien disparu : `x86_64-w64-mingw32-objdump -dr build/src/<Nom>.cpp.o | grep -i pow` (chercher une relocation `powf` — si rien ne sort, c'est bon).

## 7. Convention d'affichage des knobs

- Fréquence en Hz mais progression perceptuelle (LFO, VCO) : le param doit être **linéaire en octaves**, pas en Hz — sinon la rotation du knob est écrasée vers les hautes fréquences. Utiliser `configParam(id, minOctaves, maxOctaves, défautOctaves, "Nom", " Hz", 2.f, 1.f)` (le `2.f` = affichage exponentiel en base 2) et convertir en Hz via `approxExp2_taylor5` avant de l'utiliser (voir `SolarLFO.cpp::octavesToHz`).
- Temps d'enveloppe (attack/release/decay) : même principe mais en ms, en suivant la convention de `Fundamental`'s ADSR (`configParam(id, 0.f, 1.f, défaut, "Nom", " ms", MAX_TIME/MIN_TIME, MIN_TIME*1000.f)`).

## 8. Compiler et tester

- **Itération rapide** (juste AmbientModules, sans reconstruire tout Cardinal) : `bash /home/neil/ambientmodules-build-windows.sh` — compile contre le Rack-SDK officiel et déploie directement dans `.../AppData/Local/Rack2/plugins-win-x64/AmbientModules/`. **Fermer Rack avant** (sinon `plugin.dll` est verrouillé, erreur "Permission denied" au déploiement).
- **Test dans Cardinal lui-même** (après l'étape 4) : `bash /home/neil/cardinal-build-windows.sh` — build complet, plus long. Par défaut ne synchronise que `CardinalNative.exe` + son dossier `resources/` vers `/mnt/d/Src/Cardinal/bin` (suffisant pour tester le standalone). Pour synchroniser aussi les autres formats (VST3/CLAP/LV2/AU) avant un test en DAW : `FULL_SYNC=1 bash /home/neil/cardinal-build-windows.sh`.

## 9. Commit

Uniquement quand demandé explicitement — messages de commit en anglais (convention Cardinal), même si les échanges se font en français.
