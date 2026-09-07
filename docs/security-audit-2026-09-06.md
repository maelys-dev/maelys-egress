# Audit consolidé — 6 septembre 2026

> **Clôture au 7 septembre 2026.** Les sept points ouverts de ce document
> sont corrigés et fusionnés : A04 par la PR #31, A05 et A06 par la #36,
> A07 tranché comme décision de politique par la #37, A01 par la #33, A03
> par la #34, A02 par la #35. Les correctifs C01 à C03 déjà présents dans
> le checkout audité ont été publiés par la #30. La version 0.16.0 les
> porte tous. Le corps ci-dessous n'est pas récrit : il reste le compte
> rendu daté de l'état audité, et ses références de lignes valent pour le
> checkout de l'époque, pas pour le code actuel. Seuls les liens ont été
> repointés quand la bibliothèque a été découpée en couches, pour qu'ils
> résolvent encore.
>
> Un défaut de la même classe qu'A04, hors des sources qu'il cite, a été
> trouvé ensuite en généralisant son raisonnement à tous les champs à
> longueur explicite convertis en chaîne C : un `token_file` contenant un
> NUL était tronqué en silence aux octets précédents. Voir le journal des
> modifications.

## Périmètre et statut

Cette synthèse regroupe les audits successifs, sans nouvelle correction du
code produit. Elle porte sur le checkout local de `maelys-egress`, version
`0.15.0`, à partir du commit
`7ff1013ee1dffd4f9a4428cde54276d6fd41a6b8`, **avec les modifications non
commitées de la précédente passe de correction**. Les corrections ci-dessous
ne sont donc pas déclarées publiées.

Périmètre examiné : parsing HTTP et ClientHello, classification IPv6,
résolution du binaire des SDK Python/Node.js, événements et administration du
SDK Python. Les essais réseau sont locaux ; aucune cible de production n'a
été testée. Cette revue ciblée n'est ni un audit exhaustif des dépendances,
ni une certification d'absence de vulnérabilité.

## Registre unique des points ouverts

Les identifiants ci-dessous servent au suivi. P1 indique une priorité haute
dans le scénario indiqué ; P2 un défaut à corriger ou une décision à prendre.
La priorité ne constitue pas une mesure CVSS.

| ID | Priorité | Constat | Preuve et portée |
| --- | --- | --- | --- |
| A01 | P1 conditionnel | Découverte automatique du binaire dans des répertoires non vérifiés | Lecture de code et permissions locales ; aucune élévation de privilèges exécutée |
| A02 | P2 | Rétention illimitée des événements Python | Reproduit, y compris avec callback et connexions non authentifiées |
| A03 | P2 | Appels d'administration Python soumis au proxy d'environnement | Réponse `health()` falsifiée par un faux proxy local |
| A04 | P2 | Comparaison SNI tronquée sur NUL | Nom malformé accepté par le parseur ; pas d'accès hors allowlist démontré |
| A05 | P2 | Validation HTTP incomplète pour HTAB et DEL | Octets acceptés et transmis ; pas de request smuggling de bout en bout démontré |
| A06 | P2 | URI HTTP avec query mais sans chemin mal réécrite | `GET ?x=1` produit au lieu de `GET /?x=1` |
| A07 | P2 | Rejet IPv6 trop large dans `2001::/23` | Classification reproduite ; point de compatibilité/politique, pas une ouverture réseau |

### A01 — Confiance insuffisante dans la découverte du binaire

Sources : [Python, fonction _resolve_binary](../sdk/python/src/maelys_egress/__init__.py)
(lignes 39–47), [Node.js, fonction resolveBinary](../sdk/node/index.js) (lignes 22–32).

Les SDK essaient le répertoire de l'interpréteur, `/opt/homebrew/bin`,
`/usr/local/bin` et `/usr/bin`. Ils vérifient que le candidat est un fichier
exécutable, mais pas la confiance dans son propriétaire ou ses répertoires
parents. Le passage à un chemin absolu a supprimé la recherche dans `PATH`,
pas cette autre frontière de confiance.

Si le SDK tourne avec des privilèges supérieurs à ceux d'un utilisateur qui
peut écrire dans un répertoire candidat sélectionné, cet utilisateur peut
faire sélectionner son binaire. Sur la machine examinée, `/opt/homebrew/bin`
appartient à l'utilisateur local et a le mode `drwxrwxr-x`. Ce constat ne
prouve pas qu'un service privilégié utilise le SDK ni qu'un binaire malveillant
est installé. Aucun test n'a écrit dans ces répertoires ou exécuté de code root.

Critère de clôture : définir explicitement la politique de confiance pour
l'exécution privilégiée, puis tester le refus d'une découverte automatique
non fiable. Un chemin absolu explicite n'est pas, à lui seul, une validation
du propriétaire et de l'intégrité du binaire.

### A02 — File d'événements Python non bornée

Source : [SDK Python](../sdk/python/src/maelys_egress/__init__.py), lignes
171, 193, 216–218 et 234 (`_consume_lifecycle`, `start`).

Chaque événement est ajouté à une `queue.Queue()` sans limite, puis envoyé
au callback. Le callback ne retire pas l'événement de la file, alors qu'il
est présenté comme une alternative à `next_event()` dans la documentation.
Même l'utilisation courante sans consommation explicite conserve les reçus.

Reproduction avec un vrai daemon local : 128 connexions refusées sans
authentification, 128 callbacks de reçu exécutés, 129 événements en attente
(128 reçus et `ready`), `maxsize=0`. La rétention croît avec le nombre total de
connexions ; un épuisement mémoire à grande échelle n'a pas été provoqué.

Critère de clôture : politique de rétention bornée ou explicitement activée,
avec débordement documenté, sans bloquer la lecture de stdout du daemon.
Tester les modes callback seul, consommateur lent et absence de consommateur.

### A03 — Administration Python routée par un proxy hérité

Source : [SDK Python](../sdk/python/src/maelys_egress/__init__.py), lignes
287–303 (`_admin_get`, `health`, `metrics`) et 311–328 (suivi du rechargement).

`urlopen()` utilise les proxys d'environnement lorsque la destination locale
n'est pas exclue. Le test configure uniquement un faux proxy HTTP sur
loopback ; l'appel à `http://127.0.0.1:9/healthz` lui parvient et `health()`
retourne son JSON `{"status":"forged-by-proxy","policy_generation":999}`.
Cette seconde partie du test n'a pas de daemon d'administration : elle isole
le routage de `_admin_get` en renseignant son port de test.

L'effet reproduit est une réponse de santé falsifiée. Le même chemin est
utilisé pour les métriques et donc pour vérifier l'avancement d'une génération,
mais un faux acquittement de rechargement n'a pas été reproduit séparément.

Critère de clôture : utiliser un client local dédié sans proxy d'environnement,
et garder les redirections sous contrôle. Python documente la désactivation
par `ProxyHandler({})` dans la
[documentation urllib](https://docs.python.org/3/library/urllib.request.html#urllib.request.ProxyHandler).
Tester une configuration avec proxy actif et sans exemption loopback.

### A04 — NUL incorporé dans le SNI

Sources : [parseur ClientHello](../src/core/clienthello.c), lignes 114–117 ;
[egress_canonical_host](../src/core/common.c), lignes 44–46 ;
[passage en relais](../src/server/connection.c), lignes 195–205.

Le champ TLS a une longueur explicite. Après copie vers une chaîne C,
`strlen()` arrête toutefois la validation au premier NUL. Un ClientHello
contenant les octets `example.com\0.other.example` est accepté pour la
destination `example.com` (`result=1`). Le serveur marque alors le SNI comme
vérifié et passe en relais.

Cela contredit le refus des SNI malformés annoncé par le modèle de sécurité.
Cela ne démontre pas qu'un serveur TLS accepte ensuite ce ClientHello, ni
qu'une autre adresse que celles épinglées soit joignable.

Critère de clôture : valider tout le champ de longueur explicite avant sa
conversion en chaîne ; refuser les NUL incorporés. Tester un nom valide,
un nom différent et un suffixe après NUL.

### A05 — Caractères de contrôle HTTP encore transmis

Source : [parseur HTTP](../src/core/http.c), lignes 235–247 et 256–263.

La validation autorise HTAB partout dans l'en-tête et ne refuse pas DEL
(`0x7f`). Les cas suivants sont acceptés et conservés dans les octets transmis :

- cible `http://example.com/a\tb` → `GET /a\x09b HTTP/1.1` ;
- valeur `X: a\x7fb` → même valeur contenant DEL.

La [RFC 9112, section 3](https://www.rfc-editor.org/rfc/rfc9112.html#section-3)
interdit les espaces blancs dans la cible. La
[RFC 9110, section 5.5](https://www.rfc-editor.org/rfc/rfc9110.html#section-5.5)
encadre les valeurs de champ et leurs caractères de contrôle. HTAB peut être
licite dans certaines valeurs : il ne faut pas le bannir indistinctement.

Critère de clôture : valider séparément la ligne de requête et les champs,
refuser les caractères interdits avant transmission, et préserver les cas
HTAB licites. Le risque d'interprétation divergente justifie ce durcissement ;
une attaque request smuggling complète n'est pas établie par ces deux tests.

### A06 — Query sans chemin

Source : [parseur HTTP](../src/core/http.c), lignes 292–308 et 329–330.

`GET http://example.com?x=1 HTTP/1.1` est réécrit en
`GET ?x=1 HTTP/1.1`, sans le `/` obligatoire pour un chemin vide en
origin-form. C'est un défaut de conformité et d'interopérabilité,
indépendant des caractères de contrôle d'A05. Voir la
[RFC 9112, section 3.2.1](https://www.rfc-editor.org/rfc/rfc9112.html#section-3.2.1).

Critère de clôture : produire `/?x=1`, préserver la query et tester aussi
une URI sans chemin ni query ainsi qu'une URI avec chemin.

### A07 — Exceptions globales IPv6 rejetées

Source : [classification d'adresses](../src/core/common.c), ligne 135.

Le rejet global de `2001::/23` couvre aussi `2001:1::1`, `2001:3::1` et
`2001:4:112::1` : le test renvoie `blocked=1` pour les trois. Leurs allocations
plus spécifiques sont marquées globalement joignables dans le
[registre IANA IPv6](https://www.iana.org/assignments/iana-ipv6-special-registry/iana-ipv6-special-registry.xhtml),
consulté le 6 septembre 2026. Cette propriété ne garantit pas qu'une route ou
un service TCP existe pour chacune de ces adresses.

Ce filtrage était déjà présent avant les dernières corrections. Il s'agit
d'un faux positif par rapport à une classification des adresses globales,
pas d'un accès supplémentaire accordé à un client.

Critère de clôture : décider si le produit suit les exceptions les plus
spécifiques du registre ou assume une politique plus restrictive. Dans le
second cas, la documenter et la tester explicitement ; ne pas élargir
automatiquement les permissions sous couvert de correction.

## Corrections précédentes vérifiées dans le checkout

| ID | Correction présente | Validation | Limite de la clôture |
| --- | --- | --- | --- |
| C01 | Rejet des CR isolés dans l'en-tête HTTP | Cas `bare CR` refusé avec une erreur CRLF ; test dans `test_http_parser_adversarial` | A05 reste ouvert ; ne clôt pas tout risque de request smuggling |
| C02 | Rejet IPv6 étendu aux espaces non globaux identifiés ; traitement IPv4-mapped/NAT64 | `64:ff9b:1::1` et `100:0:0:1::1` refusés ; tests de classification enrichis | A07 reste ouvert ; pas de preuve exhaustive sur toutes les allocations |
| C03 | Suppression de la recherche de binaire par `PATH` et refus des valeurs `binary` relatives | Lecture des deux SDK et tests Python/Node.js de refus | A01 reste ouvert ; un emplacement fixe n'est pas nécessairement fiable |

## Reproductions conservées

Les deux sondes sont des pièces de diagnostic, hors de `make check` :

- [sonde C](audits/2026-09-06/probe_protocol.c) : HTTP, SNI et IPv6, sans réseau ;
- [sonde Python](audits/2026-09-06/probe_sdk.py) : événements du vrai daemon et
  faux proxy d'administration, exclusivement sur loopback, avec arrêt/nettoyage.

Elles impriment des observations. **Un exit 0 indique que la sonde a terminé,
pas que le code est exempt de défauts.** A01 n'est pas testé dynamiquement.

Depuis la racine du dépôt, adapter les trois chemins absolus suivants à des
dépendances déjà présentes et conformes aux pins :

```sh
AUDIT_BUILD=/chemin/absolu/build-audit
AUDIT_SYSTEM=/chemin/absolu/maelys-system
AUDIT_CLI=/chemin/absolu/maelys-cli
make check BUILD="$AUDIT_BUILD" MAELYS_SYSTEM_DIR="$AUDIT_SYSTEM" \
  MAELYS_CLI_DIR="$AUDIT_CLI" CC=clang CXX=clang++
clang -std=c11 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -pthread \
  -I. -Iinclude -I"$AUDIT_SYSTEM/include" \
  docs/audits/2026-09-06/probe_protocol.c \
  "$AUDIT_BUILD/lib/libmaelys_egress.a" \
  "$AUDIT_BUILD/deps/maelys-system/lib/libmaelys_sys.a" \
  -o "$AUDIT_BUILD/probe-protocol"
"$AUDIT_BUILD/probe-protocol"
"$AUDIT_BUILD/bin/maelys-egress" describe --summary --format json --compact --non-interactive
"$AUDIT_BUILD/bin/maelys-egress" describe serve --format json --non-interactive
PYTHONDONTWRITEBYTECODE=1 MAELYS_EGRESS_BINARY="$AUDIT_BUILD/bin/maelys-egress" \
  python3 docs/audits/2026-09-06/probe_sdk.py
```

Les descripteurs sont à lire avant l'appel SDK. `serve` garde son stdout de
protocole ; le SDK positionne `MAELYS_CLI_FORMAT=json` pour ses erreurs.

## Validation et limites

- Lors de la consolidation, les deux sondes conservées ont été relancées et
  ont reproduit les observations ci-dessus. La sonde C compile aussi avec
  `-Wall -Wextra -Werror`. `git diff --check` ne signale pas d'erreur.
- Dépendances utilisées : Maelys System `v0.9.0`, commit
  `6bd51950c83eaad9ec16cbac318549ab9bb2e928` ; Maelys CLI `v0.5.11`, commit
  `e347740560480da1b09f8fee6c028b4f7d1b6c03`.
- `make check` a réussi sur le code audité avec ces dépendances : tests C,
  opérations, CLI, exemples, Python (3), Node.js (3), frontières, intégration
  System, contrats, schémas et en-tête C++.
- La précédente passe de correction avait également réussi les vérifications
  ASan/UBSan et l'analyse statique Clang. Ces deux campagnes n'ont pas été
  relancées pour la seule consolidation documentaire.
- La tentative de fuzzing précédente était bloquée par l'absence du runtime
  libFuzzer dans l'Apple Clang disponible : aucune campagne de fuzzing réussie
  n'est revendiquée.
- Les tests de régression existants ne détectent pas A02 à A06. Les sondes
  complémentaires en conservent les preuves ; les transformer en tests de
  non-régression fait partie de la correction, pas de cette consolidation.
- Le checkout voisin de Maelys System ne correspondait pas au pin lors de la
  première tentative. Les validations réussies utilisent les checkouts épinglés
  isolés, pas ce checkout voisin.

Ordre proposé pour la suite : traiter A01 en priorité si un usage privilégié
existe ; corriger A02–A04, puis A05–A06 ; arbitrer A07 sans affaiblir les refus
de destinations non globales. Aucun point ouvert n'est marqué corrigé sur la
seule base d'une suite de tests verte.
