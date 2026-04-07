# External Components

Components in this folder can be added to your ESPHome project using the following yaml:

```
external_components:
  - source:
      type: git
      url: https://github.com/mahipat99/espy-common
    components: [web_server, captive_portal]
    refresh: always
```

`components:` is a comma-separated list of components that you actually want to add.  You can remove the `components:` line to import all components, but that is not recommended !
