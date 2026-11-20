"""[DOS2] dmTouch map of dmColumn_d.swf (the hybrid, type 0x8d: tools/dos2_column_touch.py) - the M2 pilot's hybrid half. The same screen
('side') and the same meanings as dmSide_p.py: a panel button opens that panel (host:panel(n) = openPanel in the hybrid, dmSide in touch),
the map, the fold arrows fold / unfold the column inside the movie (its own dmColFold + dmColApply, as its old press code does)."""

MOVIE = {
    'movie': 'dmColumn_d.swf', 'screen': 'side', 'family': 'c', 'types': [0x8d],
    'class_fn': 0xae2780,
    'hidden_at_start': ['dmVisFoldL_mc'],   # Runner only: the movie's dmColApply hides the unfold arrow while the column is shown
    'elements': [
        ('character', 'dmVis1_mc', 'e', 'panel', 0, {'open': 'host:panel(1)'}),
        ('equipment', 'dmVis5_mc', 'e', 'panel', 0, {'open': 'host:panel(5)'}),
        ('inventory', 'dmVis0_mc', 'e', 'panel', 0, {'open': 'host:panel(0)'}),
        ('skills', 'dmVis2_mc', 'e', 'panel', 0, {'open': 'host:panel(2)'}),
        ('crafting', 'dmVis3_mc', 'e', 'panel', 0, {'open': 'host:panel(3)'}),
        ('journal', 'dmVis4_mc', 'e', 'panel', 0, {'open': 'host:panel(4)'}),
        ('map', 'dmVisMap_mc', 'e', 'map', 0, {'open': 'host:map'}),
        ('fold', 'dmVisFoldR_mc', 'e', 'toggle', 40, {'toggle': ['toggle:dmColFold', 'call:.dmColApply()']}),
        ('unfold', 'dmVisFoldL_mc', 'e', 'toggle', 40, {'toggle': ['toggle:dmColFold', 'call:.dmColApply()']}),
    ],
}
