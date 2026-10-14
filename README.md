# aSaveBackup

Homebrew Switch (`.nro`) qui sauvegarde les saves Pokémon de la console sur le serveur atools
(voir `docs/pokemon-saves-api.md` dans le dépôt atools).

- **Jeux Switch** : chaque jeu du catalogue qui a un `switchTitleId` et une save pour le profil choisi.
  Le dossier de save est envoyé en ZIP déterministe (même contenu, mêmes octets).
- **melonDS** : chaque `.sav` sous `nds_dir` dont la ROM `.nds` (même nom, même dossier) a un code jeu
  présent dans `ndsGameCodes` du catalogue. Le `.sav` est envoyé tel quel.

Chaque save part sur `PUT /backups/<jeu>/<slot>`. Le serveur garde les 10 dernières versions.
L'app ne fait que lire les saves : elle n'écrit jamais sur la console.

## Compiler

Avec Docker (pas besoin d'installer devkitPro) :

```sh
docker run --rm -v "$PWD":/w -w /w devkitpro/devkita64 make
```

Le résultat est `aSaveBackup.nro`.

## Configuration

Copie `config.example.ini` vers `sdmc:/config/atools-backup/config.ini` sur la carte SD et remplis `api_key`.

| Clé | Défaut | Rôle |
| --- | --- | --- |
| `url` | `https://atools.akinaru.fr` | URL du serveur |
| `api_key` | (obligatoire) | Clé API, jamais affichée à l'écran |
| `slot` | `switch` | Nom du slot de backup (`A-Z a-z 0-9 . _ -`, 64 max) |
| `nds_dir` | `sdmc:/roms/nds` | Dossier scanné (récursif) pour les `.sav` melonDS |

Avec plusieurs profils Switch, mets un `slot` différent par profil. Sinon chaque envoi remplace la save
de l'autre profil (le serveur garde quand même les anciennes versions).

## Installer et lancer avec DBI

DBI est un homebrew d'installation et de gestion de fichiers. Ici, il sert à **copier** le `.nro` et la
config sur la carte SD par USB, sans retirer la carte. DBI ne lance pas les `.nro` : le lancement se fait
depuis le menu homebrew (hbmenu).

### 1. Préparer le Mac

macOS ne lit pas le MTP nativement. Installe un client MTP, par exemple
[OpenMTP](https://openmtp.ganeshrvel.com/) (gratuit).

### 2. Ouvrir la carte SD avec DBI

1. Sur la Switch, ouvre le menu homebrew (Album, ou un jeu lancé en maintenant R).
2. Lance **DBI**.
3. Choisis **Run MTP responder**.
4. Branche la Switch au Mac en USB-C et ouvre OpenMTP.
5. La Switch apparaît avec plusieurs entrées. Ouvre **SD Card** : c'est la racine `sdmc:/`.

### 3. Copier les fichiers

| Fichier du projet | Destination sur la SD |
| --- | --- |
| `aSaveBackup.nro` | `/switch/aSaveBackup/aSaveBackup.nro` |
| `config.example.ini` (rempli) | `/config/atools-backup/config.ini` |

Crée les dossiers s'ils n'existent pas. Attends la fin de la copie, puis quitte DBI avec B ou +.

### 4. Lancer l'app

1. Ouvre le menu homebrew.
2. Lance **aSaveBackup**.
3. Choisis ton profil quand la Switch le demande.

La connexion Wi-Fi doit être active.

### Mettre à jour

Recompile, puis refais l'étape 3 en écrasant `aSaveBackup.nro`. La config reste en place.

## Commandes

| Touche | Action |
| --- | --- |
| Haut / Bas | Choisir une save |
| A | Envoyer la save sélectionnée |
| X | Envoyer toutes les saves nouvelles ou modifiées |
| + | Quitter |

États affichés : `NOUVELLE` (absente du serveur), `MODIFIEE` (différente du serveur), `A JOUR`,
`ENVOYEE`, `ERREUR` (message à droite).

## Serveur

Les saves melonDS demandent le champ `ndsGameCodes` dans `GET /games`. Le patch est dans
`server/atools-nds-game-codes.patch`. Sans lui, les saves DS sont ignorées.
