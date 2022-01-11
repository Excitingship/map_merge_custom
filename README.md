# map_merge_custom

Add correct tf tree for map_merge package

*It can only work in have_init_pose work mode, and map size should bigger than the area you would explore.*

You can use launch/map_merge.launch to setup parameter, but map size or other parameters about maps themselves can only be set by slam package like gmapping or cartographer.

You can setup initial poses in your own launch file like below, the map_merge_custom package would check and use these parameters automatically.

```xml
<group ns="$(arg robot1name)/map_merge">
    <param name="init_pose_x"   value="$(arg first_tb3_x_pos)"/>
    <param name="init_pose_y"   value="$(arg first_tb3_y_pos)"/>
    <param name="init_pose_z"   value="$(arg first_tb3_z_pos)"/>
    <param name="init_pose_yaw" value="$(arg first_tb3_yaw)"  />
  </group>
```

