#!/usr/bin/env bash

(
  cd firmware || exit 1

  pio run -t clean

  targets=(-t upload)
  if [[ "$1" == "-m" || "$1" == "--monitor" || "$1" == "monitor" ]]; then
      targets+=(-t monitor)
  fi

  pio run "${targets[@]}"
)

# back in the original directory here, even if pio crashed

