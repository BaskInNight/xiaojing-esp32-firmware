idf_component_register(
    SRCS "wash_planner.c" "wash_recipe.c"
    INCLUDE_DIRS "include"
    REQUIRES
        machine_types
        machine_config
)
