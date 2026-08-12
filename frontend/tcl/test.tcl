read_lef ../test_data/stripe_15layer.lef
set a [design_abstract_id 0]
set t [create_terminal -abstract $a -name HELLO_TCL -direction INPUT]
set p [create_terminal_port -terminal $t]
set s [create_terminal_port_shape -port $p -layer M8]
add_shape_rect -shape $s -rect { 0.0 0.0 700 700 }
terminal_port_properties $p
