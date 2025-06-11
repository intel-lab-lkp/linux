#!/usr/bin/env python3
# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
"""Interactive perf list."""

import argparse
from typing import Dict
import perf
from textual import on
from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal, HorizontalGroup, Vertical, VerticalScroll
from textual.screen import ModalScreen
from textual.widgets import Button, Footer, Header, Label, Sparkline, Static, Tree

class ErrorScreen(ModalScreen[bool]):
    """Pop up dialog for errors."""

    CSS="""
    ErrorScreen {
        align: center middle;
    }
    """
    def __init__(self, error: str):
        self.error = error
        super().__init__()

    def compose(self) -> ComposeResult:
        yield Button(f"Error: {self.error}", variant="primary", id="error")

    def on_button_pressed(self, event: Button.Pressed) -> None:
        self.dismiss(True)


class Counter(HorizontalGroup):
    """Two labels for a CPU and its counter value."""

    CSS="""
    Label {
        gutter: 1;
    }
    """

    def __init__(self, cpu: int) -> None:
        self.cpu = cpu
        super().__init__()

    def compose(self) -> ComposeResult:
        label = f"cpu{self.cpu}" if self.cpu >= 0 else "total"
        yield Label(label + " ")
        yield Label("0", id=f"counter_{label}")


class CounterSparkline(HorizontalGroup):
    """A Sparkline for a performance counter."""

    def __init__(self, cpu: int) -> None:
        self.cpu = cpu
        super().__init__()

    def compose(self) -> ComposeResult:
        label = f"cpu{self.cpu}" if self.cpu >= 0 else "total"
        yield Label(label)
        yield Sparkline([], summary_function=max, id=f"sparkline_{label}")


class IListApp(App):
    TITLE = "Interactive Perf List"

    BINDINGS = [
        Binding(key="q", action="quit", description="Quit the app")
    ]

    # Make the 'total' sparkline a different color.
    CSS = """
        #sparkline_total > .sparkline--min-color {
            color: $accent;
        }
        #sparkline_total > .sparkline--max-color {
            color: $accent 30%;
        }
    """

    def __init__(self, interval: float) -> None:
        self.interval = interval
        self.evlist = None
        super().__init__()


    def update_counts(self) -> None:
        if not self.evlist:
            return

        def update_count(cpu: int, count: int):
            # Update the raw count display.
            counter: Label = self.query(f"#counter_cpu{cpu}" if cpu >= 0 else "#counter_total")
            if not counter:
                return
            counter = counter.first(Label)
            counter.update(str(count))

            # Update the sparkline.
            line: Sparkline = self.query(f"#sparkline_cpu{cpu}" if cpu >= 0 else "#sparkline_total")
            if not line:
                return
            line = line.first(Sparkline)
            # If there are more events than the width, remove the front event.
            if len(line.data) > line.size.width:
                line.data.pop(0)
            line.data.append(count)
            line.mutate_reactive(Sparkline.data)

        # Update the total and each CPU counts, assume there's just 1 evsel.
        total = 0
        self.evlist.disable()
        for evsel in self.evlist:
            for cpu in evsel.cpus():
                aggr = 0
                for thread in evsel.threads():
                    counts = evsel.read(cpu, thread)
                    aggr += counts.val
                update_count(cpu, aggr)
                total += aggr
        update_count(-1, total)
        self.evlist.enable()


    def on_mount(self) -> None:
        """When App starts set up periodic event updating."""
        self.update_counts()
        self.set_interval(self.interval, self.update_counts)


    def set_pmu_and_event(self, pmu: str, event: str) -> None:
        # Remove previous event information.
        if self.evlist:
            self.evlist.disable()
            self.evlist.close()
            lines = self.query(CounterSparkline)
            for line in lines:
                line.remove()
            lines = self.query(Counter)
            for line in lines:
                line.remove()

        def pmu_event_description(pmu: str, event: str) -> str:
            """Find and format event description for {pmu}/{event}/."""
            def get_info(info: Dict[str, str], key: str):
                return (info[key] + "\n") if key in info else ""

            for p in perf.pmus():
                if p.name() != pmu:
                    continue
                for info in p.events():
                    if "name" not in info or info["name"] != event:
                        continue

                    desc = get_info(info, "topic")
                    desc += get_info(info, "event_type_desc")
                    desc += get_info(info, "desc")
                    desc += get_info(info, "long_desc")
                    desc += get_info(info, "encoding_desc")
                    return desc
            return "description"

        # Parse event, update event text and description.
        full_name = event if event.startswith(pmu) or ':' in event else f"{pmu}/{event}/"
        self.query_one("#event_name", Label).update(full_name)
        self.query_one("#event_description", Static).update(pmu_event_description(pmu, event))

        # Open the event.
        try:
            self.evlist = perf.parse_events(full_name)
            if self.evlist:
                self.evlist.open()
                self.evlist.enable()
        except:
            self.evlist = None

        if not self.evlist:
            self.push_screen(ErrorScreen(f"Failed to open {full_name}"))
            return

        # Add spark lines for all the CPUs. Note, must be done after
        # open so that the evlist CPUs have been computed by propagate
        # maps.
        lines = self.query_one("#lines")
        line = CounterSparkline(cpu=-1)
        lines.mount(line)
        for cpu in self.evlist.all_cpus():
            line = CounterSparkline(cpu)
            lines.mount(line)
        line = Counter(cpu=-1)
        lines.mount(line)
        for cpu in self.evlist.all_cpus():
            line = Counter(cpu)
            lines.mount(line)


    def compose(self) -> ComposeResult:
        def pmu_event_tree() -> Tree:
            """Create tree of PMUs with events under."""
            tree: Tree[str] = Tree("PMUs")
            tree.root.expand()
            for pmu in perf.pmus():
                pmu_name = pmu.name()
                pmu_node = tree.root.add(pmu_name, data=pmu_name)
                for event in sorted(pmu.events(), key=lambda x: x["name"]):
                    if "name" in event:
                        e = event["name"]
                        if "alias" in event:
                            pmu_node.add_leaf(f'{e} ({event["alias"]})', data=e)
                        else:
                            pmu_node.add_leaf(e, data=e)
            return tree

        yield Header()
        yield Horizontal(Vertical(pmu_event_tree(), id="events"),
                         Vertical(Label("event name", id="event_name"),
                                  Static("description", markup=False, id="event_description")
                                  ))
        yield VerticalScroll(id="lines")
        yield Footer()


    @on(Tree.NodeSelected)
    def on_tree_node_selected(self, event: Tree.NodeSelected[None]) -> None:
        if event.node.parent and event.node.parent.parent:
            assert event.node.parent.data is not None
            assert event.node.data is not None
            self.set_pmu_and_event(event.node.parent.data, event.node.data)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument('-I', '--interval', help="Counter update interval in seconds", default=0.1)
    args = ap.parse_args()
    app = IListApp(float(args.interval))
    app.run()
