import React from 'react';

// OLED dimensions
const OLED_W = 128;
const OLED_H = 64;

// Element types (from proto/enums.proto)
const GP_ELEMENT_BTN_BUTTON = 2;
const GP_ELEMENT_DIR_BUTTON = 3;
const GP_ELEMENT_LEVER = 5;

// Shape types
const GP_SHAPE_ELLIPSE = 0;
const GP_SHAPE_SQUARE = 1;

/**
 * Renders a button layout as an SVG preview, matching the OLED display.
 *
 * @param {Array} elements - Layout element array from ButtonLayoutData.js
 * @param {number} width - SVG display width (default 128)
 * @param {number} height - SVG display height (default 64)
 * @param {boolean} selected - Whether this layout is currently selected
 * @param {function} onClick - Click handler
 * @param {string} label - Layout name to display below
 */
const LayoutPreview = ({ elements = [], width = 128, height = 64, selected = false, onClick, label }) => {
	const scale = width / OLED_W;

	const renderElement = (el, i) => {
		const { type, params } = el;

		if (type === GP_ELEMENT_LEVER) {
			// Lever: params = [x, y, radiusX, radiusY, stroke, fill, dpadMode]
			const [x, y, rx, ry] = params;
			return (
				<g key={i}>
					<circle
						cx={x * scale}
						cy={y * scale}
						r={Math.max(rx, ry) * scale}
						fill="none"
						stroke="#666"
						strokeWidth={1}
					/>
					<line
						x1={(x - 3) * scale}
						y1={y * scale}
						x2={(x + 3) * scale}
						y2={y * scale}
						stroke="#999"
						strokeWidth={0.5}
					/>
					<line
						x1={x * scale}
						y1={(y - 3) * scale}
						x2={x * scale}
						y2={(y + 3) * scale}
						stroke="#999"
						strokeWidth={0.5}
					/>
				</g>
			);
		}

		if (type === GP_ELEMENT_DIR_BUTTON || type === GP_ELEMENT_BTN_BUTTON) {
			const shape = params.length > 7 ? params[7] : GP_SHAPE_ELLIPSE;

			if (shape === GP_SHAPE_SQUARE) {
				// Square: params = [x1, y1, x2, y2, stroke, fill, value, shape]
				const [x1, y1, x2, y2] = params;
				return (
					<rect
						key={i}
						x={Math.min(x1, x2) * scale}
						y={Math.min(y1, y2) * scale}
						width={Math.abs(x2 - x1) * scale}
						height={Math.abs(y2 - y1) * scale}
						fill={type === GP_ELEMENT_DIR_BUTTON ? '#555' : '#777'}
						stroke="#aaa"
						strokeWidth={0.5}
						rx={1}
					/>
				);
			} else {
				// Ellipse: params = [cx, cy, rx, ry, stroke, fill, value, shape]
				const [cx, cy, rx, ry] = params;
				return (
					<ellipse
						key={i}
						cx={cx * scale}
						cy={cy * scale}
						rx={rx * scale}
						ry={ry * scale}
						fill={type === GP_ELEMENT_DIR_BUTTON ? '#555' : '#777'}
						stroke="#aaa"
						strokeWidth={0.5}
					/>
				);
			}
		}

		return null;
	};

	return (
		<div
			style={{
				display: 'inline-block',
				cursor: onClick ? 'pointer' : 'default',
				border: selected ? '2px solid #0d6efd' : '2px solid transparent',
				borderRadius: 4,
				padding: 2,
				textAlign: 'center',
			}}
			onClick={onClick}
			title={label}
		>
			<svg
				width={width}
				height={height}
				viewBox={`0 0 ${width} ${height}`}
				style={{
					background: '#1a1a1a',
					borderRadius: 3,
					display: 'block',
				}}
			>
				{elements.map(renderElement)}
			</svg>
			{label && (
				<div
					style={{
						fontSize: '0.65em',
						color: selected ? '#0d6efd' : '#999',
						marginTop: 2,
						maxWidth: width,
						overflow: 'hidden',
						textOverflow: 'ellipsis',
						whiteSpace: 'nowrap',
					}}
				>
					{label}
				</div>
			)}
		</div>
	);
};

export default LayoutPreview;
