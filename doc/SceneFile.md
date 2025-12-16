# JSON Scene File Format

This document describes the structure of scene files that can be loaded by Donut's `Scene` class.

The scene loader looks at the file extension, and if it's `.gltf` or `.glb`, loads that model directly, otherwise treats the input as the custom scene description documented here. So, when Donut-based applications take a scene path on the command line, usually that scene can be either a single glTF model or this scene description.

The loader implementation can be found in [`Scene.cpp`](../src/engine/Scene.cpp) inside the `LoadWithThreadPool` function and other functions called from there.

## 1. Top-Level Structure

A scene file is a JSON object with the following top-level properties:

| Property      | Type            | Description  
|---------------|-----------------|--------------
| `models`      | array of string | Each string is a path to a model file (e.g., `.gltf`, `.glb`), relative to the scene file location.<br/>Currently, only glTF 2.0 models are supported.
| `graph`       | array of object | See the section on [`scene graph node objects`](#2-scene-graph-node-objects)
| `animations`  | array of object | See the section on [`animation objects`](#3-animation-objects)

## 2. Scene graph node objects

The `graph` section contains an array of node objects, where each node may contain child nodes inside its `children` property.

Every node object may have the following properties:

| Property      | Type                  | Description  
|---------------|-----------------------|--------------
| `name`        | string                | Name of the node
| `parent`      | string                | Path of the custom parent node, as a sequence of names separated by `/` starting from the scene root.<br/>Normally, graph nodes are attached to their parent node in the JSON structure, but this property allows attaching nodes to anything, including inside scene graphs of the model files.
| `model`       | integer               | Index into the `models` array. If specified, an instance of the model is created and attached at this node
| `translation` | 3-vector (*)          | Translation vector
| `rotation`    | 4-vector              | Quaternion rotation in XYZW format
| `euler`       | 3-vector              | Euler angles in radians (used if `rotation` is not present)
| `scaling`     | 3-vector              | Scaling vector
| `children`    | array of node objects | Child nodes
| `type`        | string                | Leaf type for custom node content. The custom node types are defined by `SceneTypeFactory::CreateLeaf`, which user code may override to support more node types

*Note* (*): The "N-vector" notation means an array of N numbers, or a single number that is broadcasted to every element of the vector.

Additional properties may be specified for leaf nodes, depending on the node type. The default implementation of `SceneTypeFactory` supports the following node types and their properties:

### `DirectionalLight`

Represents a basic directional light with or without area, shining along the light node's negative Z axis.

| Property      | Type                  | Description  
|---------------|-----------------------|--------------
| `color`       | 3-vector              | Color of the light, default is white [1, 1, 1]
| `irradiance`  | number                | Light intensity, defined as illuminance of surfaces lit by the light at normal direction, in lm/m^2
| `angularSize` | number                | Angular lsize of the light, in degrees, default is 0

### `PointLight`

Represents a point or spherical light source emitting light in all directions from its position.

| Property      | Type       | Description  
|---------------|------------|--------------
| `color`       | 3-vector   | Color of the light, default is white [1, 1, 1]
| `intensity`   | number     | Light intensity in candela (lm/sr)
| `radius`      | number     | Light radius in world units, default is 0 (point light)
| `range`       | number     | Maximum effective range of the light, default is infinite

### `SpotLight`

Represents a spotlight emitting a cone of light from its position with a smooth falloff between the inner and outer angles. The angles are measured from the light node's negative Z axis.

| Property      | Type       | Description  
|---------------|------------|--------------
| `color`       | 3-vector   | Color of the light, default is white [1, 1, 1]
| `intensity`   | number     | Light intensity in candela (lm/sr)
| `radius`      | number     | Light radius in world units, default is 0 (point light)
| `range`       | number     | Maximum effective range of the light, default is infinite
| `innerAngle`  | number     | Apex angle of the inner (full-bright) cone in degrees, default is 180
| `outerAngle`  | number     | Apex angle of the outer cone in degrees, default is 180

### `PerspectiveCamera`

Represents a perspective camera with specific projection parameters.

| Property      | Type       | Description  
|---------------|------------|--------------
| `verticalFov` | number     | Vertical field of view in radians, default is 1.0 (57.2 degrees)
| `aspectRatio` | number     | Aspect ratio, default is unspecified (automatic)
| `zNear`       | number     | Near clipping plane in world units, default is 1.0
| `zFar`        | number     | Far clipping plane in world units, default is no far plane (reverse infinite projection)

## 3. Animation objects

The `animations` section contains an array of animation objects, where each object defines animation channels for scene nodes or materials.

For more information, see [`KeyframeAnimation.cpp`](../src/engine/KeyframeAnimation.cpp)

### Animation object properties

| Property     | Type    | Description 
|--------------|---------|--------------
| `name`       | string  | Name of the animation
| `channels`   | array   | Animation channels


### Animation channel properties

| Property   | Type                | Description
|------------|---------------------|--------------
| `target`   | string              | Name of a single target node or `material:<name>`
| `targets`  | array of strings    | Multiple targets
| `mode`     | string              | Interpolation mode (`step`, `linear`, `slerp`, `hermite`, `catmull-rom`)
| `attribute`| string              | Animated property (`translation`, `rotation`, `scaling`, or custom property)
| `data`     | array               | Keyframes

### Keyframe properties

| Property      | Type             | Description
|---------------|------------------|--------------
| `time`        | number           | Keyframe time in seconds
| `value`       | number or array  | Value at keyframe
| `inTangent`   | number or array  | Incoming tangent value (Hermite spline interpolation only)
| `outTangent`  | number or array  | Outgoing tangent value (Hermite spline interpolation only)

## 4. Example

```json
{
	"models": [
		"glTF-Sample-Assets/Models/Sponza/glTF/Sponza.gltf",
		"glTF-Sample-Assets/Models/BrainStem/glTF/BrainStem.gltf"
	],
	"graph": [
		{
			"name": "Sponza",
			"model": 0
		},
		{
			"name": "DancingRobot1",
			"model": 1,
			"translation": [4, 0, -0.5],
			"scaling": 1.2,
			"rotation": [0, 0.7071068, 0, -0.7071068]
		},
		{
			"name": "DancingRobot2",
			"model": 1,
			"translation": [-5, 0, -0.5],
			"scaling": 1.2,
			"rotation": [0, 0.7071068, 0, 0.7071068]
		}
	]
}
```