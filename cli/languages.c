#include "cli/cli.h"

/* The human text of this program in French: the help, the manual page and
 * the text rendering of a failure, chosen by --lang, MAELYS_CLI_LANG or the
 * locale (agent-cli/v2, section 5). Each entry is a text exactly as the
 * catalog or a handler writes it in English, then its translation; a
 * template keeps its printf conversions in the same order. describe, the
 * envelopes and the lifecycle stream never change with the language.
 *
 * tests/test_languages.py requires every text of the catalog here: a text
 * changed in English and not here would be shown in English. The failures
 * below are those the handlers write; a message the library composes
 * reaches a failure as "%s" and stays in English. */
static const maelys_cli_translation_t french[] = {
    /* The program and its catalog (cli/main.c). */
    {"policy-enforced forward proxy for sandboxed workloads",
     "proxy sortant sous politique pour charges en bac à sable"},
    {"The daemon's complete configuration file (schema_version = 1).",
     "Fichier de configuration complet du démon (schema_version = 1)."},
    {"Absolute path of the program to start.",
     "Chemin absolu du programme à lancer."},
    {"Argument passed to the program, verbatim. Write the program and its "
     "arguments after --: before it, a word that starts with a dash is this "
     "command's to judge, and is refused.",
     "Argument transmis tel quel au programme. Écrivez le programme et ses "
     "arguments après -- : avant, un mot qui commence par un tiret relève de "
     "cette commande, qui le refuse."},
    {"Read every configuration key, its type and its constraints as JSON.",
     "Lit chaque clé de configuration, son type et ses contraintes, en JSON."},
    {"Check a configuration before starting anything; exit 2 lists what it "
     "violates.",
     "Vérifie une configuration avant de lancer quoi que ce soit ; le code de "
     "sortie 2 accompagne la liste de ce qu'elle enfreint."},
    {"Run the proxy until SIGINT or SIGTERM, its lifecycle events on stdout.",
     "Fait tourner le proxy jusqu'à SIGINT ou SIGTERM, ses événements de cycle "
     "de vie sur stdout."},
    {"Serve native channels on the private socket the configuration names.",
     "Sert des canaux natifs sur le socket privé que nomme la configuration."},
    {"Start a program with a channel on descriptor 4 unless the configuration "
     "names another; what follows -- is the program's own.",
     "Lance un programme avec un canal sur le descripteur 4, sauf si la "
     "configuration en nomme un autre ; ce qui suit -- appartient au programme."},
    {"Describe every supported configuration key and constraint.",
     "Décrit chaque clé de configuration prise en charge et ses contraintes."},
    {"Validate a complete configuration without opening listeners; exit 2 "
     "reports violations in data.",
     "Valide une configuration complète sans ouvrir d'écoute ; le code de "
     "sortie 2 signale des violations, décrites dans data."},
    {"Start mediation and run until SIGINT or SIGTERM; stdout carries the "
     "lifecycle JSON Lines stream.",
     "Démarre la médiation et tourne jusqu'à SIGINT ou SIGTERM ; stdout porte "
     "le flux JSON Lines du cycle de vie."},
    {"Serve native channels through a private Unix socket until SIGINT or SIGTERM; "
     "stdout carries the lifecycle JSON Lines stream.",
     "Sert des canaux natifs par un socket Unix privé jusqu'à SIGINT ou "
     "SIGTERM ; stdout porte le flux JSON Lines du cycle de vie."},
    {"Start one program with a native channel on an inherited descriptor and exit "
     "with its status, 128 + signal if a signal ended it; stdout and stderr are the "
     "program's. Not a sandbox: the program keeps its own network access.",
     "Lance un programme avec un canal natif sur un descripteur hérité et sort "
     "avec son code, 128 + signal si un signal l'a terminé ; stdout et stderr "
     "sont ceux du programme. Ce n'est pas un bac à sable : le programme garde "
     "son propre accès au réseau."},
    {"'serve' and 'channel broker' reserve stdout for maelys-egress-lifecycle/1 JSON Lines: "
     "wait for event == \"ready\", then keep draining receipts and reload "
     "events until the process exits; 'fatal' or a non-zero exit is a "
     "failure. 'config validate' exits 2 when data.valid is false and "
     "lists the violations in data.diagnostics. 'channel exec' leaves stdout "
     "and stderr to the program it starts and exits with that program's "
     "status: a JSON error envelope on stderr means the program never started, "
     "and only then is exit 1 this command's own.",
     "'serve' et 'channel broker' réservent stdout au flux JSON Lines "
     "maelys-egress-lifecycle/1 : attendez event == \"ready\", puis continuez à "
     "lire les reçus et les événements de rechargement jusqu'à la fin du "
     "processus ; 'fatal' ou une sortie non nulle est un échec. 'config "
     "validate' sort avec 2 quand data.valid vaut false et énumère les "
     "violations dans data.diagnostics. 'channel exec' laisse stdout et stderr "
     "au programme qu'il lance et sort avec le code de ce programme : une "
     "enveloppe d'erreur JSON sur stderr signifie que le programme n'a jamais "
     "démarré, et c'est seulement alors que la sortie 1 est celle de cette "
     "commande."},

    /* Failures and their hints (cli/commands.c, cli/config_file.c,
     * cli/serve.c, cli/channel_broker.c, cli/channel_exec.c). */
    {"configuration mode does not match the command",
     "le mode de la configuration ne correspond pas à la commande"},
    {"Use serve for a proxy configuration, channel broker when channel_listen_unix "
     "is set, and channel exec for channel_principal alone.",
     "Utilisez serve pour une configuration de proxy, channel broker quand "
     "channel_listen_unix est défini, et channel exec pour channel_principal seul."},
    {"Name a regular executable by its absolute path, not writable by group or "
     "others, in a directory only its owner or root can modify.",
     "Nommez un exécutable ordinaire par son chemin absolu, que ni le groupe ni "
     "les autres ne peuvent modifier, dans un répertoire que seuls son "
     "propriétaire ou root peuvent modifier."},
    {"out of memory", "mémoire épuisée"},
    {"Make the file a regular, single-link file owned by root or the "
     "daemon user, without group or world write permission, and not "
     "a symbolic link.",
     "Faites-en un fichier ordinaire à un seul lien, appartenant à root ou à "
     "l'utilisateur du démon, sans droit d'écriture pour le groupe ni pour les "
     "autres, et qui ne soit pas un lien symbolique."},
    {"Name an existing configuration file readable by the daemon.",
     "Nommez un fichier de configuration existant, lisible par le démon."},
    {"invalid channel default in configuration catalog",
     "valeur par défaut de canal invalide dans le catalogue de configuration"},
    {"Use maelys-egress-mbedtls or maelys-egress-wolfssl, or remove the key.",
     "Utilisez maelys-egress-mbedtls ou maelys-egress-wolfssl, ou retirez la clé."},
    {"%s:%lu: %s requires a TLS-enabled binary",
     "%s:%lu: %s demande un binaire compilé avec TLS"},
    {"Fix the reported line, then run 'maelys-egress config describe' for the "
     "accepted keys.",
     "Corrigez la ligne signalée, puis lancez 'maelys-egress config describe' "
     "pour les clés acceptées."},
    {"%s: missing required key: schema_version",
     "%s : clé obligatoire absente : schema_version"},
    {"Fix the configuration file, then run "
     "'maelys-egress config validate --config FILE'.",
     "Corrigez le fichier de configuration, puis lancez "
     "'maelys-egress config validate --config FILE'."},
    {"Fix the configuration and validate again.",
     "Corrigez la configuration et validez-la de nouveau."},
    {"Inspect the configuration, referenced files and listener availability.",
     "Examinez la configuration, les fichiers qu'elle référence et la "
     "disponibilité des écoutes."},
    {"Check available process/thread resources.",
     "Vérifiez les ressources disponibles en processus et en threads."},
    {"cannot create broker coordinator",
     "impossible de créer le coordinateur du broker"},
    {"Check channel socket parent ownership/mode, path availability and process resources.",
     "Vérifiez le propriétaire et le mode du répertoire parent du socket de "
     "canal, la disponibilité du chemin et les ressources du processus."},
    {"cannot read the monotonic clock", "impossible de lire l'horloge monotone"},
    {"Inspect the configuration and process resources.",
     "Examinez la configuration et les ressources du processus."},
    {"the server did not start", "le serveur n'a pas démarré"},
    {"Check process and descriptor limits.",
     "Vérifiez les limites de processus et de descripteurs."},
    {"cannot build the program's environment",
     "impossible de construire l'environnement du programme"},
    {"Check that the program can be executed by this user.",
     "Vérifiez que cet utilisateur peut exécuter le programme."},
    {"cannot create the launch coordinator",
     "impossible de créer le coordinateur de lancement"},
    {"Inspect the configuration, referenced files and process resources.",
     "Examinez la configuration, les fichiers qu'elle référence et les "
     "ressources du processus."},
};

const maelys_cli_language_t egress_cli_languages[] = {
    {"fr", french, MAELYS_CLI_COUNT(french)},
};

const size_t egress_cli_language_count = MAELYS_CLI_COUNT(egress_cli_languages);
