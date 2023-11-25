.. SPDX-License-Identifier: GPL-2.0
.. _dtscodingstyle:

=====================================
Devicetree Sources (DTS) Coding Style
=====================================

When writing Devicetree Sources (DTS) please observe below guidelines.  They
should be considered complementary to any rules expressed already in Devicetree
Specification and dtc compiler (including W=1 and W=2 builds).

Individual architectures and sub-architectures can add additional rules, making
the style stricter.

Naming and Valid Characters
---------------------------

Devicetree specification allows broader range of characters in node and
property names, but for code readability the choice shall be narrowed.

1. Node and property names are allowed to use only:

   * lowercase characters: [a-z]
   * digits: [0-9]
   * dash: -

2. Labels are allowed to use only:

   * lowercase characters: [a-z]
   * digits: [0-9]
   * underscore: _

3. Unit addresses shall use lowercase hex, without leading zeros (padding).

4. Hex values in properties, e.g. "reg", shall use lowercase hex.  The address
   part can be padded with leading zeros.

Example::

	gpi_dma2: dma-controller@800000 {
		compatible = "qcom,sm8550-gpi-dma", "qcom,sm6350-gpi-dma";
		reg = <0x0 0x00800000 0x0 0x60000>;
	}

Order of Nodes
--------------

1. Nodes within any bus, thus using unit addresses for children, shall be
   ordered incrementally by unit address.
   Alternatively for some sub-architectures, nodes of the same type can be
   grouped together (e.g. all I2C controllers one after another even if this
   breaks unit address ordering).

2. Nodes without unit addresses shall be ordered alpha-numerically by the node
   name.  For a few types of nodes, they can be ordered by the main property
   (e.g. pin configuration states ordered by value of "pins" property).

3. When extending nodes in the board DTS via &label, the entries shall be
   ordered either alpha-numerically or by keeping the order from DTSI (choice
   depending on sub-architecture).

Above ordering rules are easy to enforce during review, reduce chances of
conflicts for simultaneous additions (new nodes) to a file and help in
navigating through the DTS source.

Example::

	/* SoC DTSI */

	/ {
		cpus {
			/* ... */
		};

		psci {
			/* ... */
		};

		soc@ {
			dma: dma-controller@10000 {
				/* ... */
			};

			clk: clock-controller@80000 {
				/* ... */
			};
		};
	};

	/* Board DTS - alphabetical order */

	&clk {
		/* ... */
	};

	&dma {
		/* ... */
	};

	/* Board DTS - alternative order, keep as DTSI */

	&dma {
		/* ... */
	};

	&clk {
		/* ... */
	};

Order of Properties in Device Node
----------------------------------

Following order of properties in device nodes is preferred:

1. compatible
2. reg
3. ranges
4. Standard/common properties (defined by common bindings, e.g. without
   vendor-prefixes)
5. Vendor-specific properties
6. status (if applicable)
7. Child nodes, where each node is preceded with a blank line

The "status" property is by default "okay", thus it can be omitted.

Above order follows approach:

1. Most important properties start the node: compatible then bus addressing to
   match unit address.
2. Each node will have common properties in similar place.
3. Status is the last information to annotate that device node is or is not
   finished (board resources are needed).

Example::

	/* SoC DTSI */

	device_node: device-class@6789abc {
		compatible = "vendor,device";
		reg = <0x0 0x06789abc 0x0 0xa123>;
		ranges = <0x0 0x0 0x06789abc 0x1000>;
		#dma-cells = <1>;
		clocks = <&clock_controller 0>, <&clock_controller 1>;
		clock-names = "bus", "host";
		vendor,custom-property = <2>;
		status = "disabled";

		child_node: child-class@100 {
			reg = <0x100 0x200>;
			/* ... */
		};
	};

	/* Board DTS */

	&device_node {
		vdd-supply = <&board_vreg1>;
		status = "okay";
	}

Indentation
-----------

1. Use indentation according to :ref:`codingstyle`.
2. For arrays spanning across lines, it is preferred to align the continued
   entries with opening < from the first line.
3. Each entry in arrays with multiple cells (e.g. "reg" with two IO addresses)
   shall be enclosed in <>.

Example::

	thermal-sensor@c271000 {
		compatible = "qcom,sm8550-tsens", "qcom,tsens-v2";
		reg = <0x0 0x0c271000 0x0 0x1000>,
		      <0x0 0x0c222000 0x0 0x1000>;
	};

Organizing DTSI and DTS
-----------------------

The DTSI and DTS files shall be organized in a way representing the common
(and re-usable) parts of the hardware.  Typically this means organizing DTSI
and DTS files into several files:

1. DTSI with contents of the entire SoC (without nodes for hardware not present
   on the SoC).
2. If applicable: DTSI with common or re-usable parts of the hardware (e.g.
   entire System-on-Module).
3. DTS representing the board.

Hardware components which are present on the board shall be placed in the
board DTS, not in the SoC or SoM DTSI.  A partial exception is a common
external reference SoC-input clock, which could be coded as a fixed-clock in
the SoC DTSI with its frequency provided by each board DTS.
