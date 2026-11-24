# The backdrop fixture: what `scene` puts on the master layer, and what a
# later statement naming its tag does to it (mwc.c do_scene, bg_to_slot).
# `make check` diffs the compiled code against expect.dump. Not a story;
# the lines say what is on the stage.

label start:
    # Taller than the stage: a slot over black, bottom-centre, which the
    # re-show then moves from.
    scene park
    with fade
    "park, its bottom."
    show park:
        yoffset 740
    with move
    "park, glided to its top."

    # Full-stage: the backdrop. An unplaced re-show of another full-stage
    # image of its tag swaps it under the sprites, which stay.
    scene miwaroom
    show miwa
    with dissolve
    "miwaroom, miwa in front."
    show miwaroom night with dissolve
    "miwaroom night, miwa still in front."

    # Placed: the backdrop becomes the bottom slot first, under a `with
    # None`, so that the move glides it.
    show miwaroom night:
        xoffset 100
    with move
    "miwaroom night glided right, miwa in front."

    # The same after another change since the say: no `with None`, and a
    # warning that it does not glide.
    scene miwaroom
    "miwaroom alone."
    show miwa
    show miwaroom:
        xoffset 100
    with move
    "miwaroom right, miwa in front."

    # Hidden by its tag: black under the sprites.
    scene miwaroom
    show miwa
    with dissolve
    "miwaroom, miwa in front."
    hide miwaroom
    with dissolve
    "black, miwa in front."

    # Something shown behind it: the backdrop becomes a slot, above it.
    scene miwaroom
    "miwaroom alone."
    show miwa behind miwaroom
    with dissolve
    "miwaroom, miwa behind it."
    return
