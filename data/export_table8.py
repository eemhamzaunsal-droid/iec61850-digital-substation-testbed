"""Recompute the values reported in Table 8 (Figures 8 and 9) and write them to CSV.

Two operating states of the IEEE 14-bus model are solved with the same network
builder used by the testbed (digital_substation.py):

  normal     load at Bus 5 = 47.8 MW / -3.9 Mvar, CB1 closed
  post-trip  load at Bus 5 = 150 MW / 50 Mvar, CB1 open (overcurrent scenario)

CT1 sits on the breaker side of Feeder-1, so it reads the line current while
CB1 is closed and 0 A while CB1 is open. The line itself can still draw a small
charging current from the Bus 5 side after the trip; that value is reported in
the line4_i_a column.

Usage:
    python data/export_table8.py            # writes data/table8_snapshot.csv
"""

import csv
import os
import sys

import pandapower as pp

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)

from digital_substation import create_digital_substation_net  # noqa: E402

BUSBAR_V = 1000.0  # kV -> V


def solve(p_mw, q_mvar, closed):
    net = create_digital_substation_net()

    load_idx = net.load[net.load.name == "Yuk 1"].index[0]
    net.load.at[load_idx, "p_mw"] = p_mw
    net.load.at[load_idx, "q_mvar"] = q_mvar

    switch_idx = net.switch[net.switch.name == "CB1"].index[0]
    net.switch.at[switch_idx, "closed"] = closed

    pp.runpp(
        net,
        consider_isolated_bus=True,
        numba=False,
        max_iteration=100,
        init="flat",
    )

    line_idx = net.line[net.line.name == "Fider 1 Hatti"].index[0]

    bus3_v = net.res_bus.at[2, "vm_pu"] * net.bus.at[2, "vn_kv"] * BUSBAR_V
    bus4_v = net.res_bus.at[3, "vm_pu"] * net.bus.at[3, "vn_kv"] * BUSBAR_V
    line4_i = net.res_line.at[line_idx, "i_ka"] * BUSBAR_V
    ct1_i = line4_i if closed else 0.0

    return {
        "bus3_v": round(bus3_v, 1),
        "bus4_v": round(bus4_v, 1),
        "line4_i_a": round(line4_i, 1),
        "ct1_i_a": round(ct1_i, 1),
    }


def main():
    rows = [
        ("normal", "47.8 MW / -3.9 Mvar, CB1 closed", "closed", solve(47.8, -3.9, True)),
        ("post_trip", "150 MW / 50 Mvar, CB1 open", "open", solve(150.0, 50.0, False)),
    ]

    out = os.path.join(HERE, "table8_snapshot.csv")
    with open(out, "w", newline="", encoding="utf-8") as fh:
        writer = csv.writer(fh)
        writer.writerow(
            ["state", "condition", "cb1", "bus3_v", "bus4_v", "line4_i_a", "ct1_i_a"]
        )
        for state, condition, cb1, values in rows:
            writer.writerow(
                [
                    state,
                    condition,
                    cb1,
                    values["bus3_v"],
                    values["bus4_v"],
                    values["line4_i_a"],
                    values["ct1_i_a"],
                ]
            )

    print("pandapower", pp.__version__)
    print("written:", out)
    for state, condition, cb1, values in rows:
        print(f"  {state:10s} CB1={cb1:6s} Bus3={values['bus3_v']:9.1f} V "
              f"Bus4={values['bus4_v']:9.1f} V CT1={values['ct1_i_a']:6.1f} A")


if __name__ == "__main__":
    main()
