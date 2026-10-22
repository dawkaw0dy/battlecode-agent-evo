Movement: shortest and safest paths, avoiding enemy spawn tiles, walls, holes and occupied tiles. Hidden
units in bushes can block a step: the move then returns false and you can try another tile. Dashes may
cross holes, and teleport crystals ignore terrain. Use the SDK's movement masks and distance fields rather than your own walking rules.
