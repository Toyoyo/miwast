# The branching fixture: every form of jump, menu, `if` and variable the
# compiler accepts, so that the path check (mwc.c) plays each of them in
# every combination. Not a story; the lines say where they are.

define m = Character("Miwa")
define r = Character(_("Ritsuha"))
define CHEAT = False
define START = 3

default trust = 0
default met = False
default persistent.seen_end = None
default name = "Personne"
default persistent.word = ""

label start:
    scene miwaroom
    show miwa
    with fade
    m "Début. confiance [trust], vu la fin [persistent.seen_end], triche [CHEAT]."

    # A say caption; branches that leave the stage alike, so they meet at
    # once after the menu.
    menu:
        m "Première question ?"
        "Oui":
            $ met = True
            $ trust = START
            $ name = "Clémence \"{i}[x]{/i}\""
            m "Oui, donc."
        "Non":
            $ met = False
            $ name = ''
            m "Non, donc."

    m "Après la première question, rencontrée [met]."

    # Strings: printed as they are (tags read, brackets not), compared by
    # their text, ordered, false when empty.
    m "Nom : [name]."
    if name:
        m "Un nom."
    if name == "Clémence \"{i}[x]{/i}\"" and name != "Personne":
        m "Le nom du oui."
    if name > "B" and name <= "D":
        m "Entre B et D."

    # A bare caption and three options, one conditional; the branches
    # leave the stage different, so what follows is compiled twice until
    # the scene.
    menu:
        "Deuxième question, sans personne pour la poser."
        "Montrer Ritsuha":
            show ritsuha at right
            r "Me voici."
        "Montrer Miwa qui parle" if met:
            show miwa talk
            m "Je parle."
        "Ne rien montrer" if not met and trust == 0:
            m "Rien."

    m "Entre les deux scènes."
    show miwa cry
    m "Miwa pleure, quoi qu'il en soit."

    scene miwapark
    with dissolve
    m "Après la scène, tout se rejoint."

    # if / elif / else, comparisons and `and`/`or`/`not`.
    if trust > 2 and met:
        m "Confiance haute."
        $ trust = trust
    elif trust == 0 or not met:
        m "Confiance nulle ou pas rencontrée."
    else:
        m "Autre cas."

    if CHEAT:
        m "Jamais dit : la triche est une constante fausse."

    if trust != START:
        play music "audio/watashi-no-oto.mp3"
        m "Musique dans une branche seulement."

    m "La musique dépend du chemin."
    stop music

    # A menu with no caption whose options can all be excluded: with none
    # offered, the game carries on past it.
    menu:
        "Seulement si rencontrée" if met and trust == START:
            m "Option offerte."
        "Jamais offerte" if CHEAT:
            m "Jamais dit."

    m "Après le menu qui peut ne rien offrir."

    # A string in persistent. Here, past the menus check-st's rollbacks and
    # loads cross: what they cross is not in the compiler's walk, and a
    # persistent value set there stays set (Ren'Py's persistent is not in
    # the rollback log), so the two would differ. Not `name or "rien"`:
    # the reference evaluator (minirenpy's expr.c) gives `or` a 0 or 1,
    # where Python gives the operand.
    m "Le mot était [persistent.word]."
    $ persistent.word = name
    if not name:
        $ persistent.word = "rien"
    m "Le mot est [persistent.word]."

    # Jumps forward and back, and a loop left by a menu.
    $ trust = 0
    jump ahead

label behind:
    m "Derrière : confiance [trust]."
    menu:
        "Encore":
            $ trust = 1
            jump ahead
        "Assez":
            jump done

label ahead:
    m "Devant."
    menu:
        "Retourner derrière":
            jump behind
        "Finir":
            pass

label done:
    # A menu inside an `if`, and one inside a menu. The branches set the
    # title differently and meet before it is shown.
    $ title = "Sans titre"
    if met:
        menu:
            "Imbriqué dans un if : A":
                $ title = "Titre A"
                menu:
                    "Imbriqué dans un menu : A1":
                        m "A1."
                    "Imbriqué dans un menu : A2":
                        m "A2."
            "Imbriqué dans un if : B":
                $ title = "Titre B"
                m "B."

    show screen title
    $ renpy.pause()
    hide screen title
    m "Le titre montré dépend du chemin."

    $ title = "Fin\nDu test"
    show screen title
    $ renpy.pause()
    hide screen title

    if _preferences.language == None:
        show fin
    if _preferences.language == "english":
        show fin

    $ persistent.seen_end = True
    m "Fin : vu la fin [persistent.seen_end], confiance [trust], rencontrée [met]."
    return
