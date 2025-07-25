# treport.py - perf report like tool written using textual
# SPDX-License-Identifier: MIT
from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.widgets import Footer, Header, TabbedContent, TabPane, Tree
from textual.widgets.tree import TreeNode
from typing import Dict

class ProfileNode:
    """Represents a single node in a call stack tree.

    Generally a ProfileNode corresponds to a symbol in a call stack.
    The root is special, its children are events and the events
    children are process names. After the process name come the
    samples.

    Attributes:
        name (str): The name of the function, process or event.
        value (int): The sample count for this node including counts from its
                     children.
        parent (ProfileNode): The parent of this node, this node belongs to its
                              children.
        children (Dict[str, ProfileNode]): A dictionary of child nodes, keyed by
                                           their names.
    """
    def __init__(self, name: str, parent: "ProfileNode"):
        """Initializes a ProfileNode."""
        self.name = name
        self.value: int = 0
        self.parent = parent if parent else self
        self.children: Dict[str, ProfileNode] = {}

    def find_or_create_node(self, name: str) -> "ProfileNode":
        """Finds a child node by name or creates it if it doesn't exist."""
        if name in self.children:
            return self.children[name]
        child = ProfileNode(name, self)
        self.children[name] = child
        return child

    def depth(self) -> int:
        """The maximum depth of the call stack tree from this node down."""
        if not self.children:
            return 1
        return max([child.depth() for child in self.children.values()]) + 1

    def process_event(self, event: Dict) -> None:
        """Processes a single profiling event to update the call stack tree.

        Args:
            event (Dict): A dictionary representing a single profiling sample,
                          expected to contain keys like 'comm', 'pid', 'period',
                          and 'callchain'.
        """
        pid = 0
        if "sample" in event and "pid" in event["sample"]:
            pid = event["sample"]["pid"]

        if pid == 0:
            comm = event.get("comm", "kernel")
        else:
            comm = f"{event.get('comm', 'unknown')} ({pid})"

        period = int(event["period"]) if 'period' in event else 1
        self.value += period

        node = self.find_or_create_node(comm)
        node.value += period

        if "callchain" in event:
            for entry in reversed(event["callchain"]):
                sym = entry.get("sym")
                name = None
                if sym:
                    name = sym.get("name")
                if not name:
                    name = entry.get("dso", "unknown")
                    if "ip" in entry:
                        name += f" 0x{entry['ip']:x}"
                node = node.find_or_create_node(name)
                node.value += period
        else:
            name = event.get("symbol")
            if not name:
                name = event.get("dso", "unknown")
                if "ip" in event:
                    name += f" 0x{event['ip']:x}"
            node = node.find_or_create_node(name)
            node.value += period

    def add_to_tree(self, node: TreeNode, root_value: int) -> None:
        """Recursively adds this node and its children to a textual TreeNode.

        Args:
            node (TreeNode): The textual `TreeNode` object to which this
                             ProfileNode should be added.
            root_value (int): Value at the root of the tree.
        """
        if root_value == 0:
            root_value = self.value

        # Calculate the percentage for the node, highlighting the
        # percentage with reversed colors.
        if root_value != 0:
            percent = self.value / root_value * 100
            label = f"{self.name} [r]{percent:.3g}%[/]"
        else:
            label = self.name

        # Add a standalone leaf.
        if not self.children:
            node.add_leaf(label)
            return

        # Recursively add children.
        new_node = node.add(label)
        for pnode in sorted(self.children.values(),
                            key=lambda pnode: pnode.value, reverse=True):
            pnode.add_to_tree(new_node, root_value)


class ReportApp(App):
    """A Textual application to display profiling data."""

    # The ^q binding is implied but having it here adds it in the Footer.
    BINDINGS = [
        Binding(key="^q", action="quit", description="Quit",
                tooltip="Quit the app"),
    ]

    def __init__(self, root: ProfileNode):
        """Initialize the application."""
        super().__init__()
        self.root = root

    def make_report_tree(self) -> Tree:
        """Make a Tree widget from the profile data."""
        tree: Tree[None] = Tree("Profile")
        # Add events to tree skipping the root.
        for pnode in sorted(self.root.children.values(),
                            key=lambda node: node.value, reverse=True):
            pnode.add_to_tree(tree.root, root_value=0)
        # Expand the first 2 levels of the tree.
        tree.root.expand()
        for tnode in tree.root.children:
            tnode.expand()
        return tree

    def compose(self) -> ComposeResult:
        """Composes the user interface of the application."""
        yield Header()
        with TabbedContent(initial="report"):
            with TabPane("Report", id="report"):
                yield self.make_report_tree()
        yield Footer()


class ProfileBuilder:
    """Constructs a profile tree from a stream of events."""
    def __init__(self):
        self.root = ProfileNode("root", parent=None)

    def process_event(self, event) -> None:
        """Called by `perf script` to update the profile tree."""
        ev_name = event.get("ev_name", "default")
        ev_root = self.root.find_or_create_node(ev_name)
        ev_root.process_event(event)

if __name__ == "__main__":
    # process_event is called for each perf event to build the profile.
    profile = ProfileBuilder()
    process_event = profile.process_event
    # trace_end will run the application, this can't be done
    # concurrently as perf expects to be the main thread as does
    # Textual.
    app = ReportApp(profile.root)
    trace_end = app.run
